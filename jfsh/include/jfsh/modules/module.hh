#pragma once
// jfsh/modules/module.hh — loadable module interface.
//
// Mirrors zsh's module.c / Src/Modules/*.  Modules are dynamically loaded
// shared objects that can register additional builtins, parameters, and
// condition codes.  `zmodload` and `autoload -m` are the user-facing entry
// points.

#include <jfsh/core/types.hh>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jfsh {

struct BuiltinDef;   // forward — defined in builtins.hh
struct ExecContext;  // forward — defined in executor.hh

// ---------------------------------------------------------------------------
// ModuleFeatures — what a module registers
// ---------------------------------------------------------------------------
struct ModuleFeatures {
    std::vector<BuiltinDef>  builtins;    // extra builtins
    // params, conds, math functions — extended as needed
};

// ---------------------------------------------------------------------------
// ModuleBootstrap — entry-point a module .so must export
// ---------------------------------------------------------------------------
// Conventions (mirrors zsh):
//   int   setup_(Module m);    — called when module is loaded
//   int   boot_(Module m);     — called after setup
//   int   cleanup_(Module m);  — called before unload
//   int   finish_(Module m);   — called during unload
// We wrap this via ModuleHandle.

// ---------------------------------------------------------------------------
// ModuleHandle — represents one loaded module
// ---------------------------------------------------------------------------
struct ModuleHandle {
    std::string name;        // e.g. "zsh/terminfo"
    std::string path;        // resolved .so path
    void*       dlhandle = nullptr;
    bool        auto_   = false;  // loaded via autoload

    // Symbols resolved from the .so
    using SetupFn   = int(*)(void*);
    using BootFn    = int(*)(void*);
    using CleanupFn = int(*)(void*);
    using FinishFn  = int(*)(void*);

    SetupFn   setup_fn   = nullptr;
    BootFn    boot_fn    = nullptr;
    CleanupFn cleanup_fn = nullptr;
    FinishFn  finish_fn  = nullptr;

    ModuleFeatures features;
};

// ---------------------------------------------------------------------------
// ModuleManager
// ---------------------------------------------------------------------------
class ModuleManager {
public:
    // Load a module by name; looks up module search path.
    Result<ModuleHandle*> load(std::string_view name, bool autoload = false);

    // Unload a loaded module.
    Result<void> unload(std::string_view name);

    // Check if a module is loaded.
    bool is_loaded(std::string_view name) const noexcept;

    // List all loaded modules.
    void for_each(std::function<void(const ModuleHandle&)>) const;

    // Module search path (mirrors $module_path).
    void add_search_path(std::string path);
    const std::vector<std::string>& search_paths() const noexcept { return paths_; }

    // Get combined features from all loaded modules.
    std::vector<const BuiltinDef*> all_builtins() const;

private:
    HashTable<std::unique_ptr<ModuleHandle>> modules_;
    std::vector<std::string>                 paths_;

    std::string resolve_path(std::string_view name) const;
};

} // namespace jfsh
