#ifndef JFSH_MODULES_MODULE_H
#define JFSH_MODULES_MODULE_H
// jfsh/modules/module.h — loadable module interface.
//
// Mirrors zsh's module.c / Src/Modules/*.  Modules are dynamically loaded
// shared objects that can register additional builtins, parameters, and
// condition codes.  `zmodload` and `autoload -m` are the user-facing entry
// points.

#include <cstdint>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct BuiltinDef;   // forward — defined in builtins.h
struct ExecContext;  // forward — defined in executor.h

struct ModuleFeatures {
  vec<BuiltinDef> builtins;
};

struct ModuleHandle {
  using SetupFn   = int(*)(void*);
  using BootFn    = int(*)(void*);
  using CleanupFn = int(*)(void*);
  using FinishFn  = int(*)(void*);

  str   name;
  str   path;
  void* dlhandle  = nullptr;
  bool  auto_     = false;    // loaded via autoload

  SetupFn     setup_fn   = nullptr;
  BootFn      boot_fn    = nullptr;
  CleanupFn   cleanup_fn = nullptr;
  FinishFn    finish_fn  = nullptr;

  ModuleFeatures features;
};

struct ModuleManager {
  Result<s::reference_wrapper<ModuleHandle>>
  load(strv name, bool autoload = false);

  Result<void> unload(strv name);

  bool is_loaded(strv name) const noexcept {
    return mModules.count(str(name)) > 0;
  }

  void for_each(func<void(const ModuleHandle&)> fn) const {
    for (const auto& [name, h] : mModules) fn(*h);
  }

  void add_search_path(str path) { mPaths.push_back(s::move(path)); }

  const vec<str>& search_paths() const noexcept {
    return mPaths;
  }

  vec<const BuiltinDef*> all_builtins() const;

protected:
  str resolve_path(strv name) const;

private:
  HashTable<uptr<ModuleHandle>> mModules;
  vec<str>                      mPaths;
};

}  // namespace jfsh

#endif  // JFSH_MODULES_MODULE_H
