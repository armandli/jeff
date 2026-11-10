#include <jfsh/builtins/builtins.hh>
#include <jfsh/exec/executor.hh>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace jfsh {

// ---------------------------------------------------------------------------
// BuiltinTable
// ---------------------------------------------------------------------------

void BuiltinTable::register_all() {
    // POSIX special builtins
    add({":",        builtin_colon,      BINF_SPECIAL});
    add({".",        builtin_dot,        BINF_SPECIAL, 1});
    add({"break",    builtin_break,      BINF_SPECIAL});
    add({"continue", builtin_continue,   BINF_SPECIAL});
    add({"eval",     builtin_eval,       BINF_SPECIAL});
    add({"exec",     builtin_exec,       BINF_SPECIAL});
    add({"exit",     builtin_exit,       BINF_SPECIAL});
    add({"export",   builtin_export,     BINF_SPECIAL | BINF_ASSIGN});
    add({"readonly", builtin_readonly,   BINF_SPECIAL | BINF_ASSIGN});
    add({"return",   builtin_return,     BINF_SPECIAL});
    add({"set",      builtin_set,        BINF_SPECIAL});
    add({"shift",    builtin_shift,      BINF_SPECIAL});
    add({"times",    builtin_times,      BINF_SPECIAL});
    add({"trap",     builtin_trap,       BINF_SPECIAL});
    add({"unset",    builtin_unset,      BINF_SPECIAL});
    // Directory
    add({"cd",       builtin_cd,         BINF_NONE, 0, 2});
    add({"pushd",    builtin_pushd,      BINF_NONE});
    add({"popd",     builtin_popd,       BINF_NONE});
    add({"dirs",     builtin_dirs,       BINF_NONE});
    add({"pwd",      builtin_pwd,        BINF_NONE});
    // Job control
    add({"bg",       builtin_bg,         BINF_NONE});
    add({"fg",       builtin_fg,         BINF_NONE});
    add({"jobs",     builtin_jobs,       BINF_NONE});
    add({"wait",     builtin_wait,       BINF_NONE});
    add({"disown",   builtin_disown,     BINF_NONE});
    add({"kill",     builtin_kill,       BINF_NONE});
    // Variables
    add({"typeset",  builtin_typeset,    BINF_TYPESET | BINF_ASSIGN});
    add({"local",    builtin_local,      BINF_TYPESET | BINF_ASSIGN});
    add({"declare",  builtin_declare,    BINF_TYPESET | BINF_ASSIGN});
    add({"integer",  builtin_integer,    BINF_TYPESET | BINF_ASSIGN});
    add({"float",    builtin_float,      BINF_TYPESET | BINF_ASSIGN});
    // Output
    add({"echo",     builtin_echo,       BINF_PRINT | BINF_ECHOOPT});
    add({"print",    builtin_print,      BINF_PRINT});
    add({"printf",   builtin_printf,     BINF_PRINT});
    // Input
    add({"read",     builtin_read,       BINF_NONE});
    // History
    add({"fc",       builtin_fc,         BINF_NONE});
    add({"history",  builtin_history,    BINF_NONE});
    add({"r",        builtin_r,          BINF_NONE});
    // Misc
    add({"alias",    builtin_alias,      BINF_NONE});
    add({"unalias",  builtin_unalias,    BINF_NONE});
    add({"builtin",  builtin_builtin,    BINF_NONE});
    add({"command",  builtin_command,    BINF_NONE});
    add({"source",   builtin_source,     BINF_NONE, 1});
    add({"hash",     builtin_hash,       BINF_NONE});
    add({"rehash",   builtin_rehash,     BINF_NONE});
    add({"true",     builtin_true,       BINF_NONE});
    add({"false",    builtin_false,      BINF_NONE});
    add({"[",        builtin_test,       BINF_NONE});
    add({"test",     builtin_test,       BINF_NONE});
    add({"ulimit",   builtin_ulimit,     BINF_NONE});
    add({"umask",    builtin_umask,      BINF_NONE});
    add({"zmodload", builtin_zmodload,   BINF_NONE});
    add({"autoload", builtin_autoload,   BINF_NONE});
    add({"functions",builtin_functions,  BINF_NONE});
    add({"whence",   builtin_whence,     BINF_NONE});
    add({"which",    builtin_which,      BINF_NONE});
    add({"type",     builtin_type,       BINF_NONE});
    add({"compinit", builtin_compinit,   BINF_NONE});
    add({"setopt",   builtin_setopt,     BINF_NONE});
    add({"unsetopt", builtin_unsetopt,   BINF_NONE});
    add({"bindkey",  builtin_bindkey,    BINF_NONE});
    add({"vared",    builtin_vared,      BINF_NONE});
    add({"zle",      builtin_zle,        BINF_NONE});
    add({"let",      builtin_let,        BINF_NONE});
    add({"sched",    builtin_sched,      BINF_NONE});
}

void BuiltinTable::add(BuiltinDef def) {
    table_.emplace(def.name, std::move(def));
}

const BuiltinDef* BuiltinTable::find(std::string_view name) const noexcept {
    auto it = table_.find(std::string(name));
    return it != table_.end() ? &it->second : nullptr;
}

void BuiltinTable::for_each(std::function<void(const BuiltinDef&)> fn) const {
    for (const auto& [name, def] : table_) fn(def);
}

// ---------------------------------------------------------------------------
// Stub implementations (return 0/1; full impl TBD)
// ---------------------------------------------------------------------------

int builtin_colon     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_break     (std::vector<std::string>&, ExecContext& ctx) {
    ctx.break_level = 1; return 0;
}
int builtin_continue  (std::vector<std::string>&, ExecContext& ctx) {
    ctx.cont_level = 1; return 0;
}
int builtin_dot       (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_eval      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_exec      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_exit      (std::vector<std::string>& args, ExecContext& ctx) {
    ctx.exiting  = true;
    ctx.exit_val = args.size() > 1 ? std::atoi(args[1].c_str()) : ctx.last_exit;
    return ctx.exit_val;
}
int builtin_export    (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_readonly  (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_return    (std::vector<std::string>& args, ExecContext& ctx) {
    ctx.returning = true;
    ctx.return_val = args.size() > 1 ? std::atoi(args[1].c_str()) : ctx.last_exit;
    return ctx.return_val;
}
int builtin_set       (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_shift     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_times     (std::vector<std::string>&, ExecContext&) {
    std::printf("0m0.000s 0m0.000s\n0m0.000s 0m0.000s\n"); return 0;
}
int builtin_trap      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_unset     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_cd        (std::vector<std::string>& args, ExecContext&) {
    const char* dir = args.size() > 1 ? args[1].c_str() : getenv("HOME");
    return dir ? chdir(dir) : 1;
}
int builtin_pushd     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_popd      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_dirs      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_pwd       (std::vector<std::string>&, ExecContext&) {
    char buf[4096]; if (getcwd(buf, sizeof buf)) std::printf("%s\n", buf); return 0;
}
int builtin_bg        (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_fg        (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_jobs      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_wait      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_disown    (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_kill      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_typeset   (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_local     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_declare   (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_integer   (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_float     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_fc        (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_history   (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_r         (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_alias     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_unalias   (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_builtin   (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_command   (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_echo      (std::vector<std::string>& args, ExecContext&) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (i > 1) std::putchar(' ');
        std::fputs(args[i].c_str(), stdout);
    }
    std::putchar('\n'); return 0;
}
int builtin_print     (std::vector<std::string>& args, ExecContext& ctx) {
    return builtin_echo(args, ctx);
}
int builtin_printf    (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_read      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_source    (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_hash      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_rehash    (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_true      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_false     (std::vector<std::string>&, ExecContext&) { return 1; }
int builtin_test      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_ulimit    (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_umask     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_zmodload  (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_autoload  (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_functions (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_whence    (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_which     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_type      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_compinit  (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_setopt    (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_unsetopt  (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_bindkey   (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_vared     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_zle       (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_zpty      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_sched     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_let       (std::vector<std::string>&, ExecContext&) { return 0; }

} // namespace jfsh
