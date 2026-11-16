#include <jfsh/modules/module.h>

#include <dlfcn.h>

#include <filesystem>

#include <jfsh/builtins/builtins.h>
#include <jfsh/core/aliases.h>

namespace jfsh {

Result<s::reference_wrapper<ModuleHandle>>
ModuleManager::load(strv name, bool autoload) {
  str sname(name);
  if (mModules.count(sname))
    return s::ref(*mModules[sname]);

  str path = resolve_path(name);
  if (path.empty())
    return s::unexpected(ShellError{ErrorKind::ModuleError,
        "module not found: " + sname});

  void* dl = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
  if (not dl)
    return s::unexpected(ShellError{ErrorKind::ModuleError,
        str("dlopen: ") + dlerror()});

  auto handle = s::make_unique<ModuleHandle>();
  handle->name     = sname;
  handle->path     = path;
  handle->dlhandle = dl;
  handle->auto_    = autoload;

  handle->setup_fn   = reinterpret_cast<ModuleHandle::SetupFn>  (dlsym(dl, "setup_"));
  handle->boot_fn    = reinterpret_cast<ModuleHandle::BootFn>   (dlsym(dl, "boot_"));
  handle->cleanup_fn = reinterpret_cast<ModuleHandle::CleanupFn>(dlsym(dl, "cleanup_"));
  handle->finish_fn  = reinterpret_cast<ModuleHandle::FinishFn> (dlsym(dl, "finish_"));

  if (handle->setup_fn) handle->setup_fn(handle.get());
  if (handle->boot_fn)  handle->boot_fn(handle.get());

  ModuleHandle* raw = handle.get();
  mModules[sname] = s::move(handle);
  return s::ref(*raw);
}

Result<void> ModuleManager::unload(strv name) {
  str sname(name);
  auto it = mModules.find(sname);
  if (it == mModules.end())
    return s::unexpected(ShellError{ErrorKind::ModuleError,
        "module not loaded: " + sname});
  auto& h = *it->second;
  if (h.cleanup_fn) h.cleanup_fn(&h);
  if (h.finish_fn)  h.finish_fn(&h);
  if (h.dlhandle)   dlclose(h.dlhandle);
  mModules.erase(it);
  return {};
}

str ModuleManager::resolve_path(strv name) const {
  // Convert "zsh/terminfo" → "zsh_terminfo.so"
  str soname(name);
  for (char& c : soname) if (c == '/') c = '_';
  soname += ".so";
  for (const auto& dir : mPaths) {
    str full = dir + "/" + soname;
    if (s::filesystem::exists(full)) return full;
  }
  return {};
}

vec<const BuiltinDef*> ModuleManager::all_builtins() const {
  vec<const BuiltinDef*> result;
  for (const auto& [name, h] : mModules)
    for (const auto& b : h->features.builtins)
      result.push_back(&b);
  return result;
}

}  // namespace jfsh
