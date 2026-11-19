#include <jfsh/modules/module.h>

#include <dlfcn.h>

#include <algorithm>
#include <filesystem>

#include <jfsh/builtins/builtins.h>
#include <jfsh/core/aliases.h>

namespace jfsh {

namespace {

// zmodload chases alias chains until it finds a non-alias.  Stop after a
// few hops in case the user set `A=B B=A`.
constexpr int MAX_ALIAS_HOPS = 16;

}  // namespace

void ModuleManager::register_builtin_module(str name, ModuleHandle::RegisterFn fn) {
  mBuiltin[s::move(name)] = s::move(fn);
}

void ModuleManager::add_alias(str alias, str target) {
  mAliases[s::move(alias)] = s::move(target);
}

void ModuleManager::remove_alias(strv alias) {
  mAliases.erase(str(alias));
}

void ModuleManager::mark_autoload(str name, str builtin) {
  if (builtin.empty()) builtin = name;
  mAutoloads[s::move(builtin)] = s::move(name);
}

opt<str> ModuleManager::autoload_for_builtin(strv builtin_name) const {
  auto it = mAutoloads.find(str(builtin_name));
  if (it == mAutoloads.end()) return s::nullopt;
  return it->second;
}

str ModuleManager::resolved(strv name) const {
  str cur(name);
  for (int i = 0; i < MAX_ALIAS_HOPS; ++i) {
    auto it = mAliases.find(cur);
    if (it == mAliases.end()) return cur;
    cur = it->second;
  }
  return cur;
}

Result<s::reference_wrapper<ModuleHandle>>
ModuleManager::load(strv name, ExecContext* ctx, bool autoload) {
  str sname = resolved(name);
  if (auto it = mModules.find(sname); it != mModules.end())
    return s::ref(*it->second);

  // Compiled-in first: cheap and always available.
  if (auto bi = mBuiltin.find(sname); bi != mBuiltin.end()) {
    if (not ctx)
      return s::unexpected(ShellError{ErrorKind::ModuleError,
          "failed to load module: " + sname + " (no shell context)"});
    auto handle   = s::make_unique<ModuleHandle>();
    handle->name  = sname;
    handle->path  = "<builtin>";
    handle->auto_ = autoload;
    handle->register_fn = bi->second;
    handle->register_fn(handle->features, *ctx);
    // Honour `zmodload -F NAME -feat` by pruning disabled features before
    // they go live.  A feature name must have been present in the
    // ModuleFeatures.feature_names declared by the register callback, else
    // the user's `-F -feat` request is silently ignored (as in zsh).
    if (auto* off = disabled_features(sname); off and not off->empty()) {
      auto drop = [&](auto& list, auto getter) {
        list.erase(
            s::remove_if(list.begin(), list.end(),
                         [&](const auto& item) { return off->count(getter(item)) > 0; }),
            list.end());
      };
      drop(handle->features.builtins,  [](const BuiltinDef&  b){ return str(b.name); });
      drop(handle->features.mathfuncs, [](const MathFuncDef& m){ return m.name; });
      drop(handle->features.conds,     [](const CondDef&     c){ return c.name; });
    }
    // Fold math funcs into the live registry.
    for (auto& f : handle->features.mathfuncs)
      mMathFuncs.add(f);
    ModuleHandle* raw = handle.get();
    mModules[sname] = s::move(handle);
    return s::ref(*raw);
  }

  // Fall back to dlopen.
  str path = resolve_path(sname);
  if (path.empty())
    return s::unexpected(ShellError{ErrorKind::ModuleError,
        "failed to load module: " + sname});

  void* dl = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
  if (not dl)
    return s::unexpected(ShellError{ErrorKind::ModuleError,
        str("failed to load module `") + sname + "': " + dlerror()});

  auto handle      = s::make_unique<ModuleHandle>();
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
  str sname = resolved(name);
  auto it = mModules.find(sname);
  if (it == mModules.end())
    return s::unexpected(ShellError{ErrorKind::ModuleError,
        "no such module: " + sname});
  auto& h = *it->second;
  for (const auto& f : h.features.mathfuncs) mMathFuncs.remove(f.name);
  if (h.features.on_unload) h.features.on_unload();
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

vec<ModuleEntry> ModuleManager::list() const {
  vec<ModuleEntry> out;
  out.reserve(mModules.size());
  for (const auto& [name, h] : mModules)
    out.push_back({h->name, h->path, true,
                   /*builtin=*/h->dlhandle == nullptr, h->auto_});
  s::sort(out.begin(), out.end(),
          [](const auto& a, const auto& b) { return a.name < b.name; });
  return out;
}

vec<ModuleEntry> ModuleManager::list_all() const {
  HashTable<ModuleEntry> seen;
  for (const auto& [name, h] : mModules)
    seen[name] = {h->name, h->path, true,
                  h->dlhandle == nullptr, h->auto_};
  for (const auto& [name, _] : mBuiltin)
    if (not seen.count(name))
      seen[name] = {name, "<builtin>", false, true, false};
  // An autoload mark overrides builtin/not-loaded status in the listing:
  // zsh prints `zmodload -a NAME` for every autoloaded name, loaded or
  // not, builtin-registered or not.
  for (const auto& [bi, mod] : mAutoloads) {
    auto& e = seen[mod];
    if (e.name.empty()) e = {mod, "<autoload>", false, false, true};
    else if (not e.loaded) e.auto_ = true;
  }
  vec<ModuleEntry> out;
  out.reserve(seen.size());
  for (auto& [_, e] : seen) out.push_back(s::move(e));
  s::sort(out.begin(), out.end(),
          [](const auto& a, const auto& b) { return a.name < b.name; });
  return out;
}

vec<const BuiltinDef*> ModuleManager::all_builtins() const {
  vec<const BuiltinDef*> result;
  for (const auto& [name, h] : mModules)
    for (const auto& b : h->features.builtins)
      result.push_back(&b);
  return result;
}

const BuiltinDef* ModuleManager::find_builtin(strv name) const {
  for (const auto& [_, h] : mModules)
    for (const auto& b : h->features.builtins)
      if (strv(b.name) == name) return &b;
  return nullptr;
}

const CondDef* ModuleManager::find_cond(strv name) const {
  for (const auto& [_, h] : mModules)
    for (const auto& c : h->features.conds)
      if (strv(c.name) == name) return &c;
  return nullptr;
}

}  // namespace jfsh
