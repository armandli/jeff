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
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct ExecContext;  // forward — defined in executor.h

enum BuiltinFlags : s::uint32_t {
  BINF_NONE     = 0,
  BINF_SPECIAL  = 1 << 0,  // POSIX special builtin (errors exit script)
  BINF_ASSIGN   = 1 << 1,  // accepts name=value args (like typeset)
  BINF_TYPESET  = 1 << 2,  // is a typeset variant
  BINF_PRINT    = 1 << 3,  // print / echo family
  BINF_ECHOOPT  = 1 << 4,  // echo with -e / -E options
  BINF_NOGLOB   = 1 << 5,  // don't glob-expand arguments
  BINF_PSPECIAL = 1 << 6,  // pseudo-special (like `export`)
};

using BuiltinFn = func<int(vec<str>&, ExecContext&)>;

struct BuiltinDef {
  const char* name;
  BuiltinFn   fn;
  s::uint32_t flags;
  int         min_args = 0;
  int         max_args = -1;  // -1 = unlimited
};

struct BuiltinTable {
  void register_all();

  optref<const BuiltinDef> find(strv name) const noexcept;

  void add(BuiltinDef def) { mTable.emplace(def.name, s::move(def)); }

  void for_each(func<void(const BuiltinDef&)>) const;

private:
  HashTable<BuiltinDef> mTable;
};

// POSIX special builtins
int builtin_break     (vec<str>&, ExecContext&);
int builtin_colon     (vec<str>&, ExecContext&);
int builtin_continue  (vec<str>&, ExecContext&);
int builtin_dot       (vec<str>&, ExecContext&);
int builtin_eval      (vec<str>&, ExecContext&);
int builtin_exec      (vec<str>&, ExecContext&);
int builtin_exit      (vec<str>&, ExecContext&);
int builtin_export    (vec<str>&, ExecContext&);
int builtin_readonly  (vec<str>&, ExecContext&);
int builtin_return    (vec<str>&, ExecContext&);
int builtin_set       (vec<str>&, ExecContext&);
int builtin_shift     (vec<str>&, ExecContext&);
int builtin_times     (vec<str>&, ExecContext&);
int builtin_trap      (vec<str>&, ExecContext&);
int builtin_unset     (vec<str>&, ExecContext&);

// Directory stack
int builtin_cd        (vec<str>&, ExecContext&);
int builtin_pushd     (vec<str>&, ExecContext&);
int builtin_popd      (vec<str>&, ExecContext&);
int builtin_dirs      (vec<str>&, ExecContext&);

// Job control
int builtin_bg        (vec<str>&, ExecContext&);
int builtin_fg        (vec<str>&, ExecContext&);
int builtin_jobs      (vec<str>&, ExecContext&);
int builtin_wait      (vec<str>&, ExecContext&);
int builtin_disown    (vec<str>&, ExecContext&);
int builtin_kill      (vec<str>&, ExecContext&);

// Variables / typeset
int builtin_typeset   (vec<str>&, ExecContext&);
int builtin_local     (vec<str>&, ExecContext&);
int builtin_declare   (vec<str>&, ExecContext&);
int builtin_integer   (vec<str>&, ExecContext&);
int builtin_float     (vec<str>&, ExecContext&);

// History
int builtin_fc        (vec<str>&, ExecContext&);
int builtin_history   (vec<str>&, ExecContext&);
int builtin_r         (vec<str>&, ExecContext&);

// Misc
int builtin_alias     (vec<str>&, ExecContext&);
int builtin_unalias   (vec<str>&, ExecContext&);
int builtin_builtin   (vec<str>&, ExecContext&);
int builtin_command   (vec<str>&, ExecContext&);
int builtin_echo      (vec<str>&, ExecContext&);
int builtin_print     (vec<str>&, ExecContext&);
int builtin_printf    (vec<str>&, ExecContext&);
int builtin_read      (vec<str>&, ExecContext&);
int builtin_source    (vec<str>&, ExecContext&);
int builtin_hash      (vec<str>&, ExecContext&);
int builtin_rehash    (vec<str>&, ExecContext&);
int builtin_pwd       (vec<str>&, ExecContext&);
int builtin_true      (vec<str>&, ExecContext&);
int builtin_false     (vec<str>&, ExecContext&);
int builtin_test      (vec<str>&, ExecContext&);
int builtin_ulimit    (vec<str>&, ExecContext&);
int builtin_umask     (vec<str>&, ExecContext&);
int builtin_zmodload  (vec<str>&, ExecContext&);
int builtin_autoload  (vec<str>&, ExecContext&);
int builtin_functions (vec<str>&, ExecContext&);
int builtin_whence    (vec<str>&, ExecContext&);
int builtin_which     (vec<str>&, ExecContext&);
int builtin_type      (vec<str>&, ExecContext&);
int builtin_compinit  (vec<str>&, ExecContext&);
int builtin_setopt    (vec<str>&, ExecContext&);
int builtin_unsetopt  (vec<str>&, ExecContext&);
int builtin_bindkey   (vec<str>&, ExecContext&);
int builtin_vared     (vec<str>&, ExecContext&);
int builtin_zle       (vec<str>&, ExecContext&);
int builtin_zpty      (vec<str>&, ExecContext&);
int builtin_sched     (vec<str>&, ExecContext&);
int builtin_let       (vec<str>&, ExecContext&);
int builtin_emulate   (vec<str>&, ExecContext&);
int builtin_noglob    (vec<str>&, ExecContext&);
int builtin_dash      (vec<str>&, ExecContext&);

// typeset and its variants (src/builtins/vars.cpp); `implied` holds the
// options the variant stands for ("gx" for export).
int typeset_common(vec<str>& args, ExecContext& ctx, strv implied);

}  // namespace jfsh

#endif  // JFSH_BUILTINS_BUILTINS_H
