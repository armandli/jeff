#include <jfsh/exec/executor.hh>
#include <jfsh/builtins/builtins.hh>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>

namespace jfsh {

static BuiltinTable s_builtins;

Executor::Executor(ExecContext& ctx) : ctx_(ctx) {
    static bool initialized = false;
    if (!initialized) {
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
    // TODO(exec): Wire up parse_string once StringInputSource is exposed from lexer/input.cpp.
    //   Steps:
    //   1. Create StringInputSource(src)
    //   2. Create Lexer(source)
    //   3. Create Parser(lexer)
    //   4. auto prog = parser.parse()  — returns Result<ProgRef>
    //   5. if (!prog) report error and return 1
    //   6. return exec(*prog)
    (void)src;
    return 0;
}

int Executor::exec_simple(std::vector<std::string> args,
                          std::vector<std::string> assigns,
                          ExecFlags flags) {
    if (args.empty()) {
        apply_assigns(assigns, false);
        return 0;
    }
    // Try builtin
    if (auto* def = s_builtins.find(args[0])) {
        return def->fn(args, ctx_);
    }
    // Fork + exec
    return do_exec(args);
}

// ---------------------------------------------------------------------------
// Execution hierarchy stubs
// ---------------------------------------------------------------------------

int Executor::exec_list(ExecState& es, ExecFlags flags) {
    int rc = 0;
    while (!es.at_end()) {
        WordCode w = es.fetch();
        WordCode op = wc_code(w);
        if (op == WC_END) break;
        if (op == WC_LIST) {
            rc = exec_sublist(es, flags);
            if (wc_data(w) & WC_LIST_END) break;
        } else {
            break;
        }
    }
    return rc;
}

int Executor::exec_sublist(ExecState& es, ExecFlags flags) {
    int rc = exec_pline(es, flags);
    while (!es.at_end()) {
        WordCode w = es.peek();
        WordCode op = wc_code(w);
        if (op != WC_SUBLIST) break;
        es.fetch();
        WordCode flag = wc_data(w);
        if ((flag & WC_SUBLIST_AND) && rc != 0) return rc;
        if ((flag & WC_SUBLIST_OR)  && rc == 0) return rc;
        rc = exec_pline(es, flags);
    }
    return rc;
}

int Executor::exec_pline(ExecState& es, ExecFlags flags) {
    return exec_cmd(es, flags);
}

int Executor::exec_cmd(ExecState& es, ExecFlags flags) {
    if (es.at_end()) return 0;
    WordCode w = es.peek();
    WordCode op = wc_code(w);
    switch (op) {
        case WC_SIMPLE:  { es.fetch(); return exec_simple({}, {}, flags); }
        case WC_IF:      { es.fetch(); return exec_if(es);      }
        case WC_WHILE:   { es.fetch(); return exec_while(es);   }
        case WC_FOR:     { es.fetch(); return exec_for(es);     }
        case WC_CASE:    { es.fetch(); return exec_case(es);    }
        case WC_SELECT:  { es.fetch(); return exec_select(es);  }
        case WC_REPEAT:  { es.fetch(); return exec_repeat(es);  }
        case WC_SUBSH:   { es.fetch(); return exec_subsh(es, flags); }
        case WC_FUNCDEF: { es.fetch(); return exec_funcdef(es); }
        case WC_COND:    { es.fetch(); return exec_cond(es);    }
        case WC_ARITH:   { es.fetch(); return exec_arith(es);   }
        case WC_ALWAYS:  { es.fetch(); return exec_always(es);  }
        default:         return 0;
    }
}

// ---------------------------------------------------------------------------
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

// TODO(exec/expand): Word expansion pipeline (mirrors zsh's prefork/postfork):
//   1. Tilde expansion  (~, ~user)
//   2. Parameter expansion  ($var, ${var:-default}, ${#var}, ${var##pat}, etc.)
//   3. Command substitution  $(cmd), `cmd`
//   4. Arithmetic substitution  $((expr))
//   5. Word splitting  (IFS)
//   6. Glob expansion  (*, ?, [...], **/...)
//   7. Quote removal
void Executor::expand_args(ExecState&, std::vector<std::string>&) {}

// TODO(exec/assign): Apply name=value assignments from the WC_ASSIGN block.
//   With export_flag=true (from `export`), mark each param PM_EXPORTED.
//   Must handle array assignments: arr=(a b c) and hash assignments: hash=(k v).
void Executor::apply_assigns(const std::vector<std::string>&, bool) {}

bool Executor::is_builtin(std::string_view name) const noexcept {
    return s_builtins.find(name) != nullptr;
}

int Executor::call_builtin(std::string_view name,
                           std::vector<std::string>& args,
                           ExecContext& ctx) {
    if (auto* def = s_builtins.find(name)) return def->fn(args, ctx);
    return 127;
}

int Executor::call_function(std::string_view /*name*/,
                            std::vector<std::string>& /*args*/) {
    return 127;
}

int Executor::do_fork(ExecState&, ExecFlags) { return 0; }

int Executor::do_exec(std::vector<std::string>& args) {
    std::vector<const char*> argv;
    for (auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        std::fprintf(stderr, "jfsh: %s: %s\n", argv[0], std::strerror(errno));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

} // namespace jfsh
