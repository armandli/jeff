#ifndef JFSH_MODULES_MODULE_H
#define JFSH_MODULES_MODULE_H
// jfsh/modules/module.h — loadable and compiled-in module interface (M10).
//
// Mirrors zsh's Src/module.c + the Src/Modules/*.c module ABI.  A module
// contributes features — builtins, parameters, math functions, condition
// codes — to the shell.  Two kinds live side by side:
//
//   1. Compiled-in modules.  `register_builtin_module()` at startup hands
//      the manager a callback that populates `ModuleFeatures` when
//      `zmodload NAME` is run (or at shell startup for preloaded modules).
//      jfsh ships zsh/datetime and zsh/mathfunc this way; M10 phase 2
//      adds zsh/parameter, zsh/zutil, zsh/system, zsh/files, zsh/stat and
//      zsh/pcre.
//
//   2. Dynamically loaded modules.  `resolve_path()` + `dlopen()` fetch a
//      .so that defines `boot_` / `setup_` / `cleanup_` / `finish_` C
//      entry points, and the manager registers whatever features the
//      module hands back.  The default `$module_path` is set by main().
//
// The user-facing surface is the `zmodload` builtin (src/builtins/builtins.cpp).

#include <cstdint>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/builtins/builtins.h>  // BuiltinDef
#include <jfsh/builtins/math.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct ExecContext;  // forward — defined in jfsh/exec/executor.h

// A math function contributed by zsh/mathfunc (and the rest of the
// mathfunc-shaped modules).  Returns the function's result, or a string
// error.  `args` is the fully-evaluated argument list (zero or more).
using MathFunc = func<Result<MathNum>(const vec<MathNum>&)>;

struct MathFuncDef {
  str      name;
  MathFunc fn;
  int      min_args = -1;   // -1 = any
  int      max_args = -1;
};

// A registry of math functions by name.  The arithmetic evaluator (math.cpp)
// looks up a bare identifier followed by `(` here.  Owned by ModuleManager,
// passed through `math_eval` by callers that have a shell context in hand.
struct MathFuncRegistry {
  void add(MathFuncDef def) { mFuncs[def.name] = s::move(def); }
  void remove(strv name)    { mFuncs.erase(str(name)); }
  const MathFuncDef* find(strv name) const noexcept {
    auto it = mFuncs.find(str(name));
    return it == mFuncs.end() ? nullptr : &it->second;
  }
  bool empty() const noexcept { return mFuncs.empty(); }
private:
  HashTable<MathFuncDef> mFuncs;
};

// Everything a module can contribute.  `builtins` holds BuiltinDef, which is
// forward-declared here and defined in jfsh/builtins/builtins.h; the vec of
// incomplete type is fine because its storage is only emitted from .cpp
// files that include the full definition.
struct ModuleFeatures {
  vec<BuiltinDef>   builtins;
  vec<MathFuncDef>  mathfuncs;
  vec<str>          params;   // names of specials this module installed
};

// A loaded module.  Compiled-in modules fill only the `str` fields,
// `features` and `register_fn`; dlopen'd modules fill `dlhandle`, the four
// `*_fn` pointers and whatever `boot_fn` wrote into `features`.
struct ModuleHandle {
  using SetupFn   = int(*)(void*);
  using BootFn    = int(*)(void*);
  using CleanupFn = int(*)(void*);
  using FinishFn  = int(*)(void*);

  // A compiled-in module's register callback.  Called when the module is
  // loaded; writes the features the shell should enable into `out`.  The
  // `ctx` argument is the live shell context, for parameters that need to
  // read/write shell state (e.g. $EPOCHSECONDS).
  using RegisterFn = func<void(ModuleFeatures& out, ExecContext& ctx)>;

  str   name;
  str   path;              // "<builtin>" for compiled-in
  void* dlhandle = nullptr;
  bool  auto_    = false;  // loaded via autoload

  SetupFn     setup_fn   = nullptr;
  BootFn      boot_fn    = nullptr;
  CleanupFn   cleanup_fn = nullptr;
  FinishFn    finish_fn  = nullptr;
  RegisterFn  register_fn{};   // compiled-in

  ModuleFeatures features;
};

// The result of `zmodload -l` / `zmodload -lL` for one module.
struct ModuleEntry {
  str  name;
  str  path;         // "<builtin>" for compiled-in
  bool loaded  = false;
  bool builtin = false;
  bool auto_   = false;
};

struct ModuleManager {
  // Register a compiled-in module.  Idempotent; later calls replace the
  // register callback.  The module is not loaded until `load()` is called.
  void register_builtin_module(str name, ModuleHandle::RegisterFn fn);

  // `zmodload -A alias=target`: `load(alias)` finds `target` instead.
  void add_alias(str alias, str target);
  void remove_alias(strv alias);
  const HashTable<str>& aliases() const noexcept { return mAliases; }

  // `zmodload -a name [builtin]`: mark `name` as autoloadable so the first
  // use of `builtin` loads it.  An empty builtin registers the module's
  // own name as the trigger.
  void mark_autoload(str name, str builtin = {});
  opt<str> autoload_for_builtin(strv builtin_name) const;

  // `zmodload [-i] name`: load (or confirm loaded) a module.  Returns a
  // reference to the handle on success; a ShellError with zsh's wording on
  // failure.  `autoload=true` means the load came from `-a`.
  Result<s::reference_wrapper<ModuleHandle>>
  load(strv name, ExecContext* ctx = nullptr, bool autoload = false);

  Result<void> unload(strv name);

  bool is_loaded(strv name) const noexcept {
    return mModules.count(resolved(name)) > 0;
  }

  // Loaded modules in zsh's listing order (name-sorted).
  vec<ModuleEntry> list() const;

  // Everything the manager knows about (loaded + compiled-in but unloaded
  // + aliases + autoloads), for `zmodload -L`.
  vec<ModuleEntry> list_all() const;

  // True iff `name` is a registered compiled-in module (loaded or not).
  bool is_builtin(strv name) const noexcept {
    return mBuiltin.count(resolved(name)) > 0;
  }

  void for_each(func<void(const ModuleHandle&)> fn) const {
    for (const auto& [name, h] : mModules) fn(*h);
  }

  void add_search_path(str path) { mPaths.push_back(s::move(path)); }
  const vec<str>& search_paths() const noexcept { return mPaths; }

  // Union of every loaded module's math function registry.  The shell
  // passes a pointer into `math_eval` so the evaluator can resolve
  // `sqrt(2)`-style calls.
  const MathFuncRegistry& math_funcs() const noexcept { return mMathFuncs; }

  // Union of every loaded module's builtins (for registering with the
  // BuiltinTable at load time; the manager keeps a copy too).
  vec<const BuiltinDef*> all_builtins() const;

  // Find a builtin contributed by a loaded module.  Returns nullptr when
  // no loaded module provides `name`.
  const BuiltinDef* find_builtin(strv name) const;

protected:
  str resolve_path(strv name) const;
  str resolved(strv name) const;  // apply alias chain; no load

private:
  HashTable<uptr<ModuleHandle>>       mModules;
  HashTable<ModuleHandle::RegisterFn> mBuiltin;   // compiled-in registry
  HashTable<str>                      mAliases;   // alias → target
  HashTable<str>                      mAutoloads; // builtin → module
  MathFuncRegistry                    mMathFuncs;
  vec<str>                            mPaths;
};

}  // namespace jfsh

#endif  // JFSH_MODULES_MODULE_H
