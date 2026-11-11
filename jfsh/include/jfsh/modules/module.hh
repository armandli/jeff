#ifndef JFSH_MODULES_MODULE_HH
#define JFSH_MODULES_MODULE_HH
// jfsh/modules/module.hh — loadable module interface.
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
#include <vector>

#include <jfsh/core/types.hh>

namespace jfsh {

struct BuiltinDef;   // forward — defined in builtins.hh
struct ExecContext;  // forward — defined in executor.hh

struct ModuleFeatures {
  std::vector<BuiltinDef> builtins;
};

struct ModuleHandle {
  using SetupFn   = int(*)(void*);
  using BootFn    = int(*)(void*);
  using CleanupFn = int(*)(void*);
  using FinishFn  = int(*)(void*);

  std::string name;
  std::string path;
  void*       dlhandle  = nullptr;
  bool        auto_     = false;    // loaded via autoload

  SetupFn     setup_fn   = nullptr;
  BootFn      boot_fn    = nullptr;
  CleanupFn   cleanup_fn = nullptr;
  FinishFn    finish_fn  = nullptr;

  ModuleFeatures features;
};

struct ModuleManager {
  Result<std::reference_wrapper<ModuleHandle>>
  load(std::string_view name, bool autoload = false);

  Result<void> unload(std::string_view name);

  bool is_loaded(std::string_view name) const noexcept;

  void for_each(std::function<void(const ModuleHandle&)>) const;

  void add_search_path(std::string path);

  const std::vector<std::string>& search_paths() const noexcept {
    return mPaths;
  }

  std::vector<const BuiltinDef*> all_builtins() const;

protected:
  std::string resolve_path(std::string_view name) const;

private:
  HashTable<std::unique_ptr<ModuleHandle>> mModules;
  std::vector<std::string>                 mPaths;
};

}  // namespace jfsh

#endif  // JFSH_MODULES_MODULE_HH
