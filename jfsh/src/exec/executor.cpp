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

int Executor::exec_string(std::string_view /*src*/) {
    // TODO: parse src and exec
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

// Stub compound commands
int Executor::exec_if     (ExecState&)         { return 0; }
int Executor::exec_while  (ExecState&)         { return 0; }
int Executor::exec_for    (ExecState&)         { return 0; }
int Executor::exec_case   (ExecState&)         { return 0; }
int Executor::exec_select (ExecState&)         { return 0; }
int Executor::exec_repeat (ExecState&)         { return 0; }
int Executor::exec_subsh  (ExecState&, ExecFlags) { return 0; }
int Executor::exec_funcdef(ExecState&)         { return 0; }
int Executor::exec_cond   (ExecState&)         { return 0; }
int Executor::exec_arith  (ExecState&)         { return 0; }
int Executor::exec_always (ExecState&)         { return 0; }

void Executor::setup_redirects  (ExecState&)                    {}
void Executor::expand_args      (ExecState&, std::vector<std::string>&) {}
void Executor::apply_assigns    (const std::vector<std::string>&, bool) {}

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
