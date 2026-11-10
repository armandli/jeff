#include <jfsh/modules/module.hh>
#include <jfsh/builtins/builtins.hh>
#include <dlfcn.h>
#include <filesystem>

namespace jfsh {

Result<ModuleHandle*> ModuleManager::load(std::string_view name, bool autoload) {
    std::string sname(name);
    if (modules_.count(sname))
        return modules_[sname].get();

    std::string path = resolve_path(name);
    if (path.empty())
        return std::unexpected(ShellError{ErrorKind::ModuleError,
            "module not found: " + sname});

    void* dl = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!dl)
        return std::unexpected(ShellError{ErrorKind::ModuleError,
            std::string("dlopen: ") + dlerror()});

    auto handle = std::make_unique<ModuleHandle>();
    handle->name     = sname;
    handle->path     = path;
    handle->dlhandle = dl;
    handle->auto_   = autoload;

    // Resolve entry points
    handle->setup_fn   = reinterpret_cast<ModuleHandle::SetupFn>  (dlsym(dl, "setup_"));
    handle->boot_fn    = reinterpret_cast<ModuleHandle::BootFn>   (dlsym(dl, "boot_"));
    handle->cleanup_fn = reinterpret_cast<ModuleHandle::CleanupFn>(dlsym(dl, "cleanup_"));
    handle->finish_fn  = reinterpret_cast<ModuleHandle::FinishFn> (dlsym(dl, "finish_"));

    if (handle->setup_fn) handle->setup_fn(handle.get());
    if (handle->boot_fn)  handle->boot_fn(handle.get());

    ModuleHandle* raw = handle.get();
    modules_[sname] = std::move(handle);
    return raw;
}

Result<void> ModuleManager::unload(std::string_view name) {
    std::string sname(name);
    auto it = modules_.find(sname);
    if (it == modules_.end())
        return std::unexpected(ShellError{ErrorKind::ModuleError,
            "module not loaded: " + sname});
    auto& h = *it->second;
    if (h.cleanup_fn) h.cleanup_fn(&h);
    if (h.finish_fn)  h.finish_fn(&h);
    if (h.dlhandle)   dlclose(h.dlhandle);
    modules_.erase(it);
    return {};
}

bool ModuleManager::is_loaded(std::string_view name) const noexcept {
    return modules_.count(std::string(name)) > 0;
}

void ModuleManager::for_each(std::function<void(const ModuleHandle&)> fn) const {
    for (const auto& [name, h] : modules_) fn(*h);
}

void ModuleManager::add_search_path(std::string path) {
    paths_.push_back(std::move(path));
}

std::string ModuleManager::resolve_path(std::string_view name) const {
    // Convert "zsh/terminfo" → "zsh_terminfo.so"
    std::string soname(name);
    for (char& c : soname) if (c == '/') c = '_';
    soname += ".so";
    for (const auto& dir : paths_) {
        std::string full = dir + "/" + soname;
        if (std::filesystem::exists(full)) return full;
    }
    return {};
}

std::vector<const BuiltinDef*> ModuleManager::all_builtins() const {
    std::vector<const BuiltinDef*> result;
    for (const auto& [name, h] : modules_)
        for (const auto& b : h->features.builtins)
            result.push_back(&b);
    return result;
}

} // namespace jfsh
