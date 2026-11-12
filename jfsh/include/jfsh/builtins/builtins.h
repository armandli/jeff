#ifndef JFSH_BUILTINS_BUILTINS_H
#define JFSH_BUILTINS_BUILTINS_H
// jfsh/builtins/builtins.h — shell builtin commands.
//
// Mirrors zsh's builtin.c / bltins1.c / bltins2.c.
// Each builtin is a function: (argv, ExecContext&) → exit_code.

#include <cstdint>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct ExecContext;  // forward — defined in executor.h

enum BuiltinFlags : std::uint32_t {
  BINF_NONE     = 0,
  BINF_SPECIAL  = 1 << 0,  // POSIX special builtin (errors exit script)
  BINF_ASSIGN   = 1 << 1,  // accepts name=value args (like typeset)
  BINF_TYPESET  = 1 << 2,  // is a typeset variant
  BINF_PRINT    = 1 << 3,  // print / echo family
  BINF_ECHOOPT  = 1 << 4,  // echo with -e / -E options
  BINF_NOGLOB   = 1 << 5,  // don't glob-expand arguments
  BINF_PSPECIAL = 1 << 6,  // pseudo-special (like `export`)
};

using BuiltinFn = std::function<int(std::vector<std::string>&, ExecContext&)>;

struct BuiltinDef {
  const char*   name;
  BuiltinFn     fn;
  std::uint32_t flags;
  int           min_args = 0;
  int           max_args = -1;  // -1 = unlimited
};

struct BuiltinTable {
  void register_all();

  std::optional<std::reference_wrapper<const BuiltinDef>>
  find(std::string_view name) const noexcept;

  void add(BuiltinDef def);

  void for_each(std::function<void(const BuiltinDef&)>) const;

private:
  HashTable<BuiltinDef> mTable;
};

// POSIX special builtins
int builtin_break     (std::vector<std::string>&, ExecContext&);
int builtin_colon     (std::vector<std::string>&, ExecContext&);
int builtin_continue  (std::vector<std::string>&, ExecContext&);
int builtin_dot       (std::vector<std::string>&, ExecContext&);
int builtin_eval      (std::vector<std::string>&, ExecContext&);
int builtin_exec      (std::vector<std::string>&, ExecContext&);
int builtin_exit      (std::vector<std::string>&, ExecContext&);
int builtin_export    (std::vector<std::string>&, ExecContext&);
int builtin_readonly  (std::vector<std::string>&, ExecContext&);
int builtin_return    (std::vector<std::string>&, ExecContext&);
int builtin_set       (std::vector<std::string>&, ExecContext&);
int builtin_shift     (std::vector<std::string>&, ExecContext&);
int builtin_times     (std::vector<std::string>&, ExecContext&);
int builtin_trap      (std::vector<std::string>&, ExecContext&);
int builtin_unset     (std::vector<std::string>&, ExecContext&);

// Directory stack
int builtin_cd        (std::vector<std::string>&, ExecContext&);
int builtin_pushd     (std::vector<std::string>&, ExecContext&);
int builtin_popd      (std::vector<std::string>&, ExecContext&);
int builtin_dirs      (std::vector<std::string>&, ExecContext&);

// Job control
int builtin_bg        (std::vector<std::string>&, ExecContext&);
int builtin_fg        (std::vector<std::string>&, ExecContext&);
int builtin_jobs      (std::vector<std::string>&, ExecContext&);
int builtin_wait      (std::vector<std::string>&, ExecContext&);
int builtin_disown    (std::vector<std::string>&, ExecContext&);
int builtin_kill      (std::vector<std::string>&, ExecContext&);

// Variables / typeset
int builtin_typeset   (std::vector<std::string>&, ExecContext&);
int builtin_local     (std::vector<std::string>&, ExecContext&);
int builtin_declare   (std::vector<std::string>&, ExecContext&);
int builtin_integer   (std::vector<std::string>&, ExecContext&);
int builtin_float     (std::vector<std::string>&, ExecContext&);

// History
int builtin_fc        (std::vector<std::string>&, ExecContext&);
int builtin_history   (std::vector<std::string>&, ExecContext&);
int builtin_r         (std::vector<std::string>&, ExecContext&);

// Misc
int builtin_alias     (std::vector<std::string>&, ExecContext&);
int builtin_unalias   (std::vector<std::string>&, ExecContext&);
int builtin_builtin   (std::vector<std::string>&, ExecContext&);
int builtin_command   (std::vector<std::string>&, ExecContext&);
int builtin_echo      (std::vector<std::string>&, ExecContext&);
int builtin_print     (std::vector<std::string>&, ExecContext&);
int builtin_printf    (std::vector<std::string>&, ExecContext&);
int builtin_read      (std::vector<std::string>&, ExecContext&);
int builtin_source    (std::vector<std::string>&, ExecContext&);
int builtin_hash      (std::vector<std::string>&, ExecContext&);
int builtin_rehash    (std::vector<std::string>&, ExecContext&);
int builtin_pwd       (std::vector<std::string>&, ExecContext&);
int builtin_true      (std::vector<std::string>&, ExecContext&);
int builtin_false     (std::vector<std::string>&, ExecContext&);
int builtin_test      (std::vector<std::string>&, ExecContext&);
int builtin_ulimit    (std::vector<std::string>&, ExecContext&);
int builtin_umask     (std::vector<std::string>&, ExecContext&);
int builtin_zmodload  (std::vector<std::string>&, ExecContext&);
int builtin_autoload  (std::vector<std::string>&, ExecContext&);
int builtin_functions (std::vector<std::string>&, ExecContext&);
int builtin_whence    (std::vector<std::string>&, ExecContext&);
int builtin_which     (std::vector<std::string>&, ExecContext&);
int builtin_type      (std::vector<std::string>&, ExecContext&);
int builtin_compinit  (std::vector<std::string>&, ExecContext&);
int builtin_setopt    (std::vector<std::string>&, ExecContext&);
int builtin_unsetopt  (std::vector<std::string>&, ExecContext&);
int builtin_bindkey   (std::vector<std::string>&, ExecContext&);
int builtin_vared     (std::vector<std::string>&, ExecContext&);
int builtin_zle       (std::vector<std::string>&, ExecContext&);
int builtin_zpty      (std::vector<std::string>&, ExecContext&);
int builtin_sched     (std::vector<std::string>&, ExecContext&);
int builtin_let       (std::vector<std::string>&, ExecContext&);

}  // namespace jfsh

#endif  // JFSH_BUILTINS_BUILTINS_H
