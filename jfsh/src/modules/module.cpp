// The module table, features, autoloads and the zmodload builtin in this
// file are derived from zsh 5.9's Src/module.c.  zsh's copyright and
// licence:
//
// Copyright (c) 1996-1997 Zoltán Hidvégi
// All rights reserved.
//
// Permission is hereby granted, without written agreement and without
// license or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall Zoltán Hidvégi or the Zsh Development Group be liable
// to any party for direct, indirect, special, incidental, or consequential
// damages arising out of the use of this software and its documentation,
// even if Zoltán Hidvégi and the Zsh Development Group have been advised of
// the possibility of such damage.
//
// Zoltán Hidvégi and the Zsh Development Group specifically disclaim any
// warranties, including, but not limited to, the implied warranties of
// merchantability and fitness for a particular purpose.  The software
// provided hereunder is on an "as is" basis, and Zoltán Hidvégi and the
// Zsh Development Group have no obligation to provide maintenance,
// support, updates, enhancements, or modifications.

#include <jfsh/modules/module.h>

#include <dlfcn.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>

#include <algorithm>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>

namespace jfsh {

// A feature to turn on (`+f`, `f`) or off (`-f`), or with zmodload -m the
// pattern the rest of the word is (zsh's struct feature_enables).
struct FeatureEnable {
  str                  text;
  opt<CompiledPattern> pat;
};

namespace {

// find_module's flags.
constexpr int FINDMOD_ALIASP = 1 << 0;   // resolve aliases
constexpr int FINDMOD_CREATE = 1 << 1;   // make the entry when missing

// autofeatures' and the add/del functions' flags.
constexpr int FEAT_IGNORE    = 1 << 0;   // no errors for what is already so
constexpr int FEAT_INFIX     = 1 << 1;   // an infix condition
constexpr int FEAT_AUTOALL   = 1 << 2;   // the autoload brings all features
constexpr int FEAT_REMOVE    = 1 << 3;   // remove the autoload
constexpr int FEAT_CHECKAUTO = 1 << 4;   // check the autoloads exist

// printmodulenode's flags.
constexpr int PRINTMOD_LIST     = 1 << 0;
constexpr int PRINTMOD_EXIST    = 1 << 1;
constexpr int PRINTMOD_ALIAS    = 1 << 2;
constexpr int PRINTMOD_DEPS     = 1 << 3;
constexpr int PRINTMOD_FEATURES = 1 << 4;
constexpr int PRINTMOD_LISTALL  = 1 << 5;
constexpr int PRINTMOD_AUTO     = 1 << 6;

void warn(const str& msg) { s::fprintf(stderr, "jfsh: %s\n", msg.c_str()); }

void warnnam(strv cmd, const str& msg) {
  s::fprintf(stderr, "jfsh: %.*s: %s\n", static_cast<int>(cmd.size()), cmd.data(),
             msg.c_str());
}

// quotedzputs and nicezputs.
void qputs(strv s) { s::fputs(shell_quote(s).c_str(), stdout); }
void nputs(strv s) {
  for (char ch : s) {
    auto c = static_cast<unsigned char>(ch);
    if (c == '\n')                  s::fputs("\\n", stdout);
    else if (c == '\t')             s::fputs("\\t", stdout);
    else if (c < 0x20 or c == 0x7f) s::printf("^%c", c ^ 0x40);
    else                            s::putchar(c);
  }
}

// Letters, digits, `_` and `/` (zsh's modname_ok).
bool modname_ok(strv p) {
  for (char c : p)
    if (not (s::isalnum(static_cast<unsigned char>(c)) or c == '_' or c == '/')) return false;
  return true;
}

bool contains(const vec<str>& v, strv s) {
  return s::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

Result<const MathFuncDef*> MathFuncRegistry::find(strv name) const {
  if (auto it = mFuncs.find(str(name)); it != mFuncs.end()) return &it->second;
  auto at = s::find_if(mMarks.begin(), mMarks.end(), [&](const Mark& m) { return m.name == name; });
  if (at == mMarks.end()) return nullptr;
  Mark mark = *at;
  mMarks.erase(at);
  if (autoload) autoload(mark);
  if (auto it = mFuncs.find(str(name)); it != mFuncs.end()) return &it->second;
  // zsh's zerr here stops callmathfunc's "unknown function".
  return s::unexpected(ShellError{ErrorKind::ExecError, "autoloading module " + mark.module
                                  + " failed to define math function: " + str(name)});
}

ModuleManager::ModuleManager() {
  mMathFuncs.autoload = [this](const MathFuncRegistry::Mark& m) {
    (void)ensure_feature(m.module, "f:", m.autoall ? nullptr : &m.name);
  };
}

ModuleManager::~ModuleManager() {
  for (auto& [name, m] : mModules)
    if (m->dlhandle) ::dlclose(m->dlhandle);
}

void ModuleManager::attach(ExecContext& ctx, func<bool(strv)> core_builtin) {
  mCtx         = &ctx;
  mCoreBuiltin = s::move(core_builtin);
  ctx.params.autoload_hook = [this](const str& name, const str& module, bool all) {
    (void)ensure_feature(module, "p:", all ? nullptr : &name);
    // zsh's getparamnode: a module that came and took the mark away
    // without making the parameter is an error, which stops the line.
    if (mCtx->params.lookup_raw(name)) return;
    warn("autoloading module " + module + " failed to define parameter: " + name);
    mCtx->raise_lookup_error();
  };
}

void ModuleManager::register_builtin_module(str name, Module::RegisterFn fn) {
  mLinked[s::move(name)] = s::move(fn);
}

// ---------------------------------------------------------------------------
// The module table.

Module* ModuleManager::find_module(strv name, int flags, str* namep) {
  str key(name);
  for (int hops = 0; hops < 64; ++hops) {
    auto it = mModules.find(key);
    if (it == mModules.end()) break;
    Module& m = *it->second;
    if ((flags & FINDMOD_ALIASP) and (m.flags & Module::ALIAS)) {
      key = m.alias;
      if (namep) *namep = key;
      continue;
    }
    if (namep) *namep = m.name;
    return &m;
  }
  if (not (flags & FINDMOD_CREATE)) return nullptr;
  auto m  = s::make_unique<Module>();
  m->name = key;
  Module* raw = m.get();
  mModules[key] = s::move(m);
  return raw;
}

const Module* ModuleManager::find_module(strv name, int flags, str* namep) const {
  return const_cast<ModuleManager*>(this)->find_module(name, flags & ~FINDMOD_CREATE, namep);
}

void ModuleManager::delete_module(strv name) { mModules.erase(str(name)); }

bool ModuleManager::is_loaded(strv name) const {
  const Module* m = find_module(name, FINDMOD_ALIASP);
  return m and m->loaded and not (m->flags & Module::UNLOAD);
}

vec<const Module*> ModuleManager::sorted_modules() const {
  vec<const Module*> out;
  out.reserve(mModules.size());
  for (const auto& [name, m] : mModules) out.push_back(m.get());
  s::sort(out.begin(), out.end(),
          [](const Module* a, const Module* b) { return a->name < b->name; });
  return out;
}

// The features a module offers, in zsh's order (featuresarray).
vec<str> ModuleManager::features_of(const Module& m) const {
  vec<str> out;
  for (const auto& b : m.features.builtins)  out.push_back("b:" + str(b.name));
  for (const auto& c : m.features.conds)     out.push_back((c.infix ? "C:" : "c:") + c.name);
  for (const auto& f : m.features.mathfuncs) out.push_back("f:" + f.name);
  for (const auto& p : m.features.params)    out.push_back("p:" + p);
  return out;
}

// Makes the module's features those `want` says (zsh's
// setfeatureenables), with zsh's messages for what fails.  Returns false
// when something did.
bool ModuleManager::set_enables(Module& m, const vec<s::uint8_t>& want) {
  // zsh's setfeatureenables names the module in its messages.
  const str nam = m.name;
  auto& f = m.features;
  m.enables.resize(features_of(m).size(), 0);
  bool ok = true;
  s::size_t i = 0;
  for (auto& b : f.builtins) {
    bool on = want[i];
    if (on != bool(m.enables[i])) {
      if (on) {
        auto it = mBuiltins.find(b.name);
        if ((mCoreBuiltin and mCoreBuiltin(b.name)) or (it != mBuiltins.end() and it->second.def)) {
          warnnam(nam, "name clash when adding builtin `" + str(b.name) + "'");
          ok = false;
        } else {
          // A module coming in makes its builtins anew, enabled (zsh frees
          // the node `disable` marked); turning one back on keeps the mark.
          if (mCtx and (m.flags & Module::SETUP)) mCtx->disabled_builtins.erase(b.name);
          mBuiltins[b.name] = {m.name, &b, false};
          m.enables[i] = 1;
        }
      } else {
        if (not mBuiltins.erase(b.name)) {
          warnnam(nam, "builtin `" + str(b.name) + "' already deleted");
          ok = false;
        }
        m.enables[i] = 0;
      }
    }
    ++i;
  }
  for (auto& c : f.conds) {
    bool on = want[i];
    if (on != bool(m.enables[i])) {
      auto at = s::find_if(mConds.begin(), mConds.end(), [&](const CondEntry& e) {
        return e.infix == c.infix and e.name == c.name;
      });
      if (on) {
        if (at != mConds.end() and at->def) {
          warnnam(nam, "name clash when adding condition `" + c.name + "'");
          ok = false;
        } else {
          if (at != mConds.end()) mConds.erase(at);
          mConds.insert(mConds.begin(), CondEntry{c.name, {}, &c, c.infix, false});
          m.enables[i] = 1;
        }
      } else {
        if (at != mConds.end()) mConds.erase(at);
        m.enables[i] = 0;
      }
    }
    ++i;
  }
  for (auto& fn : f.mathfuncs) {
    bool on = want[i];
    if (on != bool(m.enables[i])) {
      auto& marks = mMathFuncs.marks();
      if (on) {
        auto mark = s::find_if(marks.begin(), marks.end(),
                               [&](const auto& k) { return k.name == fn.name; });
        if (mark == marks.end() and mMathFuncs.has(fn.name)) {
          warnnam(nam, "name clash when adding math function `" + fn.name + "'");
          ok = false;
        } else {
          if (mark != marks.end()) marks.erase(mark);
          mMathFuncs.add(fn);
          m.enables[i] = 1;
        }
      } else {
        mMathFuncs.remove(fn.name);
        m.enables[i] = 0;
      }
    }
    ++i;
  }
  auto& params = mCtx->params;
  for (const auto& name : f.params) {
    bool on = want[i];
    if (on != bool(m.enables[i])) {
      if (on) {
        // zsh's checkaddparam.
        bool added = true;
        if (auto p = params.lookup_raw(name)) {
          if (p->get().scope > 0 or not (p->get().flags & PM_AUTOLOAD)) {
            warn("Can't add module parameter `" + name + "': "
                 + (p->get().scope > 0 ? "local parameter exists" : "parameter already exists"));
            warnnam(nam, "error when adding parameter `" + name + "'");
            ok = added = false;
          } else {
            (void)params.take_global(name);
          }
        }
        if (added) {
          if (auto st = m.stashed.find(name); st != m.stashed.end()) {
            params.put_global(s::move(st->second));
            m.stashed.erase(st);
          }
          m.enables[i] = 1;
        }
      } else {
        if (auto p = params.take_global(name)) {
          m.stashed[name] = s::move(*p);
        } else {
          warnnam(nam, "parameter `" + name + "' already deleted");
          ok = false;
        }
        m.enables[i] = 0;
      }
    }
    ++i;
  }
  return ok;
}

// zsh's do_module_features.
int ModuleManager::do_module_features(Module& m, vec<FeatureEnable>* features, int flags) {
  if (m.features.no_features or m.dlhandle) {
    if (features) {
      if (not (flags & FEAT_IGNORE))
        warn("module `" + m.name + "' does not support features");
      return 1;
    }
    return 0;
  }
  vec<str> names = features_of(m);
  vec<s::uint8_t> enables = m.enables;
  enables.resize(names.size(), 0);
  int ret = 0;
  if ((flags & FEAT_CHECKAUTO) and not m.autoloads.empty()) {
    // The autoloads this module doesn't have are dropped.
    for (s::size_t k = 0; k < m.autoloads.size();) {
      str al = m.autoloads[k];
      if (contains(names, al)) {
        ++k;
        continue;
      }
      if (not (flags & FEAT_IGNORE))
        warn("module `" + m.name + "' has no such feature: `" + al + "': autoload cancelled");
      (void)autofeatures({}, &m.name, {al}, 0, FEAT_IGNORE | FEAT_REMOVE);
      if (features) {
        for (s::size_t e = 0; e < features->size();) {
          strv text = (*features)[e].text;
          if (not text.empty() and (text[0] == '+' or text[0] == '-')) text.remove_prefix(1);
          if ((*features)[e].pat ? (*features)[e].pat->matches(al) : text == al) {
            ret = 1;
            features->erase(features->begin() + static_cast<s::ptrdiff_t>(e));
          } else {
            ++e;
          }
        }
      }
      if (k < m.autoloads.size() and m.autoloads[k] == al) ++k;
    }
  }
  if (features) {
    for (const auto& fe : *features) {
      strv esp = fe.text;
      bool on = true;
      if (not esp.empty() and esp[0] == '+') {
        esp.remove_prefix(1);
      } else if (not esp.empty() and esp[0] == '-') {
        on = false;
        esp.remove_prefix(1);
      }
      int found = 0;
      for (s::size_t i = 0; i < names.size(); ++i) {
        if (fe.pat ? fe.pat->matches(names[i]) : names[i] == esp) {
          enables[i] = on;
          ++found;
          if (not fe.pat) break;
        }
      }
      if (not found) {
        if (not (flags & FEAT_IGNORE))
          warn("module `" + m.name + "' has no "
               + (fe.pat ? "feature matching" : "such feature") + ": `" + str(esp) + "'");
        return 1;
      }
    }
  } else {
    s::fill(enables.begin(), enables.end(), 1);
  }
  set_enables(m, enables);
  if (enables != m.enables) return 2;
  return ret;
}

// zsh's setup_module: a compiled-in module's register callback makes its
// features, its parameters going to the module rather than the table (set
// up with every feature off); a dlopen'd one has setup_.
bool ModuleManager::setup_module(Module& m, bool) {
  if (m.flags & Module::LINKED) {
    auto it = mLinked.find(m.name);
    if (it == mLinked.end() or not mCtx) return false;
    m.features = {};
    m.stashed.clear();
    mCtx->params.capture(&m.stashed);
    it->second(m.features, *mCtx);
    mCtx->params.capture(nullptr);
    m.enables.assign(features_of(m).size(), 0);
    return true;
  }
  using EntryFn = int (*)(Module*);
  auto setup = reinterpret_cast<EntryFn>(::dlsym(m.dlhandle, "setup_"));
  return not setup or setup(&m) == 0;
}

int ModuleManager::do_boot_module(Module& m, vec<FeatureEnable>* features, bool silent) {
  int ret = do_module_features(m, features, silent ? FEAT_IGNORE | FEAT_CHECKAUTO : FEAT_CHECKAUTO);
  if (ret == 1) return 1;
  if (m.dlhandle) {
    using EntryFn = int (*)(Module*);
    auto boot = reinterpret_cast<EntryFn>(::dlsym(m.dlhandle, "boot_"));
    if (boot and boot(&m)) return 1;
  }
  return ret;
}

// zsh's cleanup_module and finish_module: every feature off, then what the
// module set up besides undone.
void ModuleManager::finish_module(Module& m) {
  if (not m.enables.empty()) set_enables(m, vec<s::uint8_t>(m.enables.size(), 0));
  if (m.features.on_unload) m.features.on_unload();
  m.features = {};
  m.enables.clear();
  m.stashed.clear();
  if (m.dlhandle) {
    using EntryFn = int (*)(Module*);
    if (auto cleanup = reinterpret_cast<EntryFn>(::dlsym(m.dlhandle, "cleanup_"))) cleanup(&m);
    if (auto finish = reinterpret_cast<EntryFn>(::dlsym(m.dlhandle, "finish_"))) finish(&m);
    ::dlclose(m.dlhandle);
    m.dlhandle = nullptr;
  }
  m.loaded = false;
}

// zsh's try_load_module: NAME.so dlopen'd from each $module_path
// directory in turn.  `err` gets the last failure, empty when no directory
// was tried.
void* ModuleManager::try_load_module(const str& name, str& err) const {
  if (not mCtx) return nullptr;
  auto mp = mCtx->params.lookup("module_path");
  if (not mp) return nullptr;
  for (const auto& dir : mp->get().array()) {
    str path = (dir.empty() ? str(".") : dir) + "/" + name + ".so";
    if (void* handle = ::dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL)) return handle;
    if (const char* e = ::dlerror()) err = e;
  }
  return nullptr;
}

// zsh's load_module.
int ModuleManager::load_module(str name, const vec<FeatureEnable>* features_in, bool silent) {
  vec<FeatureEnable> copy;
  vec<FeatureEnable>* features = nullptr;
  if (features_in) {
    copy     = *features_in;
    features = &copy;
  }
  if (not modname_ok(name)) {
    if (not silent) {
      warn("invalid module name `" + name + "'");
      if (mCtx) mCtx->aborting = mCtx->abort_line = true;
    }
    return 1;
  }
  // Opens the module's code: a compiled-in one is there already.
  auto open = [&](Module& m) {
    if (mLinked.contains(name)) {
      m.flags |= Module::LINKED;
      return true;
    }
    str err;
    void* handle = try_load_module(name, err);
    if (not handle) {
      if (not silent)
        warn("failed to load module `" + name + "': " + (err.empty() ? "empty module path" : err));
      return false;
    }
    m.dlhandle = handle;
    return true;
  };
  Module* m = find_module(name, FINDMOD_ALIASP, &name);
  if (not m) {
    Module fresh;
    fresh.name = name;
    if (not open(fresh)) return 1;
    auto made = s::make_unique<Module>(s::move(fresh));
    m = made.get();
    mModules[name] = s::move(made);
    m->loaded = true;
    m->flags |= Module::SETUP;
    int bootret = 0;
    bool set = setup_module(*m, silent);
    if (not set or (bootret = do_boot_module(*m, features, silent)) == 1) {
      finish_module(*m);
      delete_module(name);
      return 1;
    }
    m->flags |= Module::INIT_S | Module::INIT_B;
    m->flags &= ~Module::SETUP;
    return bootret;
  }
  if (m->flags & Module::SETUP) return 0;
  if (m->flags & Module::UNLOAD) m->flags &= ~Module::UNLOAD;
  else if (m->loaded) return 0;
  if (m->flags & Module::BUSY) {
    warn("circular dependencies for module ;" + name);
    if (mCtx) mCtx->aborting = mCtx->abort_line = true;
    return 1;
  }
  m->flags |= Module::BUSY;
  for (const str& dep : vec<str>(m->deps)) {
    if (load_module(dep, nullptr, silent) == 1) {
      m->flags &= ~Module::BUSY;
      return 1;
    }
  }
  m = find_module(name, 0);
  m->flags &= ~Module::BUSY;
  if (not m->loaded) {
    if (not open(*m)) return 1;
    m->loaded = true;
    m->flags |= Module::SETUP;
    if (not setup_module(*m, silent)) {
      finish_module(*m);
      m->flags &= ~Module::SETUP;
      return 1;
    }
    m->flags |= Module::INIT_S;
  }
  m->flags |= Module::SETUP;
  int bootret = do_boot_module(*m, features, silent);
  if (bootret == 1) {
    finish_module(*m);
    m->flags &= ~Module::SETUP;
    return 1;
  }
  m->flags |= Module::INIT_B;
  m->flags &= ~Module::SETUP;
  return bootret;
}

// zsh's require_module.
int ModuleManager::require_module(str name, vec<FeatureEnable>* features, bool silent) {
  Module* m = find_module(name, FINDMOD_ALIASP, &name);
  if (not m or not m->loaded or (m->flags & Module::UNLOAD))
    return load_module(name, features, silent);
  return do_module_features(*m, features, 0);
}

int ModuleManager::require(strv name, bool silent) {
  return require_module(str(name), nullptr, silent);
}

int ModuleManager::ensure_feature(strv module, strv prefix, const str* name) {
  if (not name) return require_module(str(module), nullptr, false);
  vec<FeatureEnable> f{{str(prefix) + *name, s::nullopt}};
  return require_module(str(module), &f, false);
}

void ModuleManager::add_dep(strv name, strv from) {
  str target(name);
  Module* m = find_module(name, FINDMOD_ALIASP | FINDMOD_CREATE, &target);
  if (not contains(m->deps, from)) m->deps.emplace_back(from);
}

void ModuleManager::autoload_features(strv module, const vec<str>& features) {
  str mod(module);
  (void)autofeatures("zsh", &mod, features, 0, FEAT_IGNORE);
}

// zsh's unload_module.
int ModuleManager::unload_module(Module& m_in) {
  Module* mp = &m_in;
  if (mp->flags & Module::ALIAS) {
    mp = find_module(mp->alias, FINDMOD_ALIASP);
    if (not mp) return 1;
  }
  Module& m = *mp;
  // zsh's cleanup_: every feature off; one that can't be (a parameter
  // the user unset) leaves the module loaded.
  if (m.loaded and not (m.flags & Module::UNLOAD) and not m.enables.empty()
      and not set_enables(m, vec<s::uint8_t>(m.enables.size(), 0)))
    return 1;
  bool del = m.flags & Module::UNLOAD;
  m.flags &= ~(Module::INIT_B | Module::INIT_S | Module::UNLOAD);
  if (m.loaded) finish_module(m);
  if (del) {
    // Unloaded late: the modules it depended on that nothing else needs
    // go too.
    for (const str& dname : vec<str>(m.deps)) {
      Module* dm = find_module(dname, FINDMOD_ALIASP);
      if (not dm or not (dm->flags & Module::UNLOAD)) continue;
      bool alone = true;
      for (const auto& [n, am] : mModules)
        if (am.get() != &m and am->loaded and contains(am->deps, dm->name)) alone = false;
      if (alone) (void)unload_module(*dm);
    }
  }
  if (not m.autoloads.empty()) {
    // Its autoloadable features come back, to load it again when needed.
    str name = m.name;
    (void)autofeatures("zsh", &name, vec<str>(m.autoloads), 0, FEAT_IGNORE);
  } else if (m.deps.empty()) {
    delete_module(m.name);
  }
  return 0;
}

// zsh's unload_named_module.
int ModuleManager::unload_named_module(const str& modname, strv cmd, bool silent) {
  str mname = modname;
  Module* m = find_module(modname, FINDMOD_ALIASP, &mname);
  if (not m) {
    if (silent) return 0;
    warnnam(cmd, "no such module " + modname);
    return 1;
  }
  for (const auto& [n, dm] : mModules) {
    if (dm->deps.empty() or not dm->loaded) continue;
    if (contains(dm->deps, mname) and not (dm->flags & Module::UNLOAD)) {
      warnnam(cmd, "module " + mname + " is in use by another module and cannot be unloaded");
      return 1;
    }
  }
  return unload_module(*m) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Autoloadable features.

int ModuleManager::add_autobin(const str& module, const str& name, int flags) {
  auto it = mBuiltins.find(name);
  if ((mCoreBuiltin and mCoreBuiltin(name)) or (it != mBuiltins.end() and it->second.def))
    return (flags & FEAT_IGNORE) ? 0 : 1;
  mBuiltins[name] = {module, nullptr, bool(flags & FEAT_AUTOALL)};
  return 0;
}

int ModuleManager::del_autobin(const str& name, int flags) {
  auto it = mBuiltins.find(name);
  if (it == mBuiltins.end()) return (flags & FEAT_IGNORE) ? 0 : 2;
  if (it->second.def or (mCoreBuiltin and mCoreBuiltin(name))) return (flags & FEAT_IGNORE) ? 0 : 3;
  mBuiltins.erase(it);
  return 0;
}

int ModuleManager::add_autocond(const str& module, const str& name, int flags) {
  bool infix = flags & FEAT_INFIX;
  auto at = s::find_if(mConds.begin(), mConds.end(),
                       [&](const CondEntry& e) { return e.infix == infix and e.name == name; });
  if (at != mConds.end()) {
    if (at->def) return (flags & FEAT_IGNORE) ? 0 : 1;
    mConds.erase(at);
  }
  mConds.insert(mConds.begin(),
                CondEntry{name, module, nullptr, infix, bool(flags & FEAT_AUTOALL)});
  return 0;
}

int ModuleManager::del_autocond(const str& name, int flags) {
  bool infix = flags & FEAT_INFIX;
  auto at = s::find_if(mConds.begin(), mConds.end(),
                       [&](const CondEntry& e) { return e.infix == infix and e.name == name; });
  if (at == mConds.end()) return (flags & FEAT_IGNORE) ? 0 : 2;
  if (at->def) return (flags & FEAT_IGNORE) ? 0 : 3;
  mConds.erase(at);
  return 0;
}

int ModuleManager::add_automathfunc(const str& module, const str& name, int flags) {
  auto& marks = mMathFuncs.marks();
  auto at = s::find_if(marks.begin(), marks.end(), [&](const auto& k) { return k.name == name; });
  if (at != marks.end()) {
    marks.erase(at);
  } else if (mMathFuncs.has(name)) {
    return (flags & FEAT_IGNORE) ? 0 : 1;
  }
  // zsh's add_automathfunc leaves out MFF_AUTOALL: the mark brings just
  // the function.
  marks.insert(marks.begin(), {name, module, false});
  return 0;
}

int ModuleManager::del_automathfunc(const str& name, int flags) {
  auto& marks = mMathFuncs.marks();
  auto at = s::find_if(marks.begin(), marks.end(), [&](const auto& k) { return k.name == name; });
  if (at != marks.end()) {
    marks.erase(at);
    return 0;
  }
  if (mMathFuncs.has(name)) return (flags & FEAT_IGNORE) ? 0 : 3;
  return (flags & FEAT_IGNORE) ? 0 : 2;
}

int ModuleManager::add_autoparam(const str& module, const str& name, int flags) {
  auto& params = mCtx->params;
  // zsh's checkaddparam.
  if (auto p = params.lookup_raw(name)) {
    bool local = p->get().scope > 0;
    if (local or not (p->get().flags & PM_AUTOLOAD)) {
      if (not (flags & FEAT_IGNORE) or local) {
        warn("Can't add module parameter `" + name + "': "
             + (local ? "local parameter exists" : "parameter already exists"));
        return -1;
      }
      return 0;
    }
    (void)params.take_global(name);
  }
  Param p;
  p.name  = name;
  p.value = module;
  p.flags = PM_SCALAR | PM_AUTOLOAD | ((flags & FEAT_AUTOALL) ? PM_AUTOALL : 0u);
  params.put_global(s::move(p));
  return 0;
}

int ModuleManager::del_autoparam(const str& name, int flags) {
  auto p = mCtx->params.lookup_raw(name);
  if (not p) return (flags & FEAT_IGNORE) ? 0 : 2;
  if (not (p->get().flags & PM_AUTOLOAD)) return (flags & FEAT_IGNORE) ? 0 : 3;
  (void)mCtx->params.take_global(name);
  return 0;
}

// zsh's autofeatures: marks (or with FEAT_REMOVE unmarks) features of
// `module` for autoload.  `prefchar` gives the type of bare names.
int ModuleManager::autofeatures(strv cmd, const str* module, const vec<str>& features,
                                char prefchar, int defflags) {
  int ret = 0;
  Module* defm = nullptr;
  vec<str> modfeatures;
  vec<s::uint8_t> modenables;
  if (module) {
    defm = find_module(*module, FINDMOD_ALIASP | FINDMOD_CREATE);
    if (defm->loaded and not defm->features.no_features and not defm->dlhandle) {
      modfeatures = features_of(*defm);
      modenables  = defm->enables;
    }
  }
  for (const str& word : features) {
    str feature, fnam;
    bool add = true;
    int flags = defflags;
    char fchar;
    if (prefchar) {
      fchar   = prefchar;
      fnam    = word;
      feature = str(1, fchar) + ":" + fnam;
    } else {
      strv f = word;
      if (not f.empty() and f[0] == '-') {
        add = false;
        f.remove_prefix(1);
      } else if (not f.empty() and f[0] == '+') {
        f.remove_prefix(1);
      }
      if (f.size() < 2 or f[1] != ':') {
        warnnam(cmd, "bad format for autoloadable feature: `" + str(f) + "'");
        ret = 1;
        continue;
      }
      feature = str(f);
      fnam    = str(f.substr(2));
      fchar   = f[0];
    }
    if (flags & FEAT_REMOVE) add = false;
    strv typnam;
    switch (fchar) {
      case 'b': typnam = "builtin";

      break; case 'C': flags |= FEAT_INFIX; [[fallthrough]];

      case 'c': typnam = "condition";

      break; case 'f': typnam = "math function";

      break; case 'p': typnam = "parameter";

      break; default:
        warnnam(cmd, str("bad autoloadable feature type: `") + fchar + "'");
        ret = 1;
        continue;
    }
    if (fnam.find('/') != str::npos) {
      warnnam(cmd, fnam + ": `/' is illegal in a " + str(typnam));
      ret = 1;
      continue;
    }
    Module* m = defm;
    if (not module) {
      // The traditional un-autoload doesn't say which module it was.
      m = nullptr;
      for (const Module* cand : sorted_modules())
        if (contains(cand->autoloads, feature)) m = const_cast<Module*>(cand);
      if (not m) {
        if (not (flags & FEAT_IGNORE)) {
          ret = 1;
          warnnam(cmd, fnam + ": no such " + str(typnam));
        }
        continue;
      }
    }
    int subret = 0;
    if (add) {
      if (not modfeatures.empty() or (defm and defm->loaded and not defm->features.no_features
                                      and not defm->dlhandle)) {
        // A module that is there must have the feature.
        auto at = s::find(modfeatures.begin(), modfeatures.end(), feature);
        if (at == modfeatures.end()) {
          warnnam(cmd, "module `" + m->name + "' has no such feature: `" + feature + "'");
          ret = 1;
          continue;
        }
        if (modenables[static_cast<s::size_t>(at - modfeatures.begin())]) continue;
      }
      // In lexical order.
      m->has_autoloads = true;
      auto pos = s::lower_bound(m->autoloads.begin(), m->autoloads.end(), feature);
      if (pos == m->autoloads.end() or *pos != feature) m->autoloads.insert(pos, feature);
    } else if (m->has_autoloads) {
      auto at = s::find(m->autoloads.begin(), m->autoloads.end(), feature);
      if (at != m->autoloads.end()) m->autoloads.erase(at);
      else subret = (flags & FEAT_IGNORE) ? -2 : 2;
    }
    if (subret == 0) {
      str modname = module ? *module : str{};
      switch (fchar) {
        case 'b':
          subret = add ? add_autobin(modname, fnam, flags) : del_autobin(fnam, flags);

        break; case 'C': case 'c':
          subret = add ? add_autocond(modname, fnam, flags) : del_autocond(fnam, flags);

        break; case 'f':
          subret = add ? add_automathfunc(modname, fnam, flags) : del_automathfunc(fnam, flags);

        break; default:
          subret = add ? add_autoparam(modname, fnam, flags) : del_autoparam(fnam, flags);
      }
    }
    if (subret != 0) {
      if (subret != -2) ret = 1;
      switch (subret) {
        case 1: warnnam(cmd, "failed to add " + str(typnam) + " `" + fnam + "'");

        break; case 2: warnnam(cmd, fnam + ": no such " + str(typnam));

        break; case 3: warnnam(cmd, fnam + ": " + str(typnam) + " is already defined");

        break; default:;
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// What the executor looks up.

const BuiltinDef* ModuleManager::find_builtin(strv name) const {
  auto it = mBuiltins.find(str(name));
  return it == mBuiltins.end() ? nullptr : it->second.def;
}

opt<str> ModuleManager::autoload_for_builtin(strv name) const {
  auto it = mBuiltins.find(str(name));
  if (it == mBuiltins.end() or it->second.def) return s::nullopt;
  return it->second.module;
}

bool ModuleManager::has_builtin(strv name) const { return mBuiltins.contains(str(name)); }

// zsh's resolvebuiltin.
const BuiltinDef* ModuleManager::resolve_builtin(strv name, bool* marked) {
  str nm(name);
  auto it = mBuiltins.find(nm);
  if (it == mBuiltins.end()) return nullptr;
  if (it->second.def) return it->second.def;
  if (marked) *marked = true;
  str module = it->second.module;
  (void)ensure_feature(module, "b:", it->second.autoall ? nullptr : &nm);
  if (const BuiltinDef* def = find_builtin(name)) return def;
  // The mark still there (the module didn't load) goes, as zsh's
  // execbuiltin drops a builtin without a handler; one taken away by
  // the module that came is an error.
  if (auto again = mBuiltins.find(nm); again != mBuiltins.end()) {
    mBuiltins.erase(again);
    return nullptr;
  }
  warn("autoloading module " + module + " failed to define builtin: " + nm);
  if (mCtx) mCtx->aborting = mCtx->abort_line = true;
  return nullptr;
}

vec<str> ModuleManager::builtin_names() const {
  vec<str> out;
  out.reserve(mBuiltins.size());
  for (const auto& [name, e] : mBuiltins) out.push_back(name);
  return out;
}

// zsh's getconddef(infix, name, 1).
const CondDef* ModuleManager::find_cond(strv name, bool infix) {
  for (bool tried = false;;) {
    auto at = s::find_if(mConds.begin(), mConds.end(),
                         [&](const CondEntry& e) { return e.infix == infix and e.name == name; });
    if (at == mConds.end()) return nullptr;
    if (at->def) return at->def;
    if (tried) {
      mConds.erase(at);
      return nullptr;
    }
    tried = true;
    str module = at->module, nm(name);
    (void)ensure_feature(module, infix ? "C:" : "c:", at->autoall ? nullptr : &nm);
  }
}

vec<s::pair<str, str>> ModuleManager::module_states() const {
  vec<s::pair<str, str>> out;
  vec<str> done;
  // An alias's name is in zsh's u.handle union: it shows as one loaded.
  for (const auto& [name, m] : mModules) {
    if ((m->loaded or (m->flags & Module::ALIAS)) and not (m->flags & Module::UNLOAD)) {
      out.emplace_back(name, (m->flags & Module::ALIAS) ? "alias:" + m->alias : str("loaded"));
      done.push_back(name);
    }
  }
  auto mark = [&](const str& mod) {
    if (contains(done, mod)) return;
    done.push_back(mod);
    out.emplace_back(mod, "autoloaded");
  };
  for (const auto& [name, e] : mBuiltins)
    if (not e.def) mark(e.module);
  for (const auto& c : mConds)
    if (not c.def) mark(c.module);
  if (mCtx)
    mCtx->params.for_each({}, [&](const Param& p) {
      if (p.flags & PM_AUTOLOAD) mark(p.scalar());
      return false;
    });
  return out;
}

// ---------------------------------------------------------------------------
// zmodload.

// zsh's printmodulenode.
void ModuleManager::print_module(const Module& m_in, int flags) const {
  const Module* m = &m_in;
  const str& modname = m_in.name;
  if (flags & PRINTMOD_DEPS) {
    if (m->deps.empty()) return;
    if (flags & PRINTMOD_LIST) {
      s::fputs("zmodload -d ", stdout);
      if (modname[0] == '-') s::fputs("-- ", stdout);
      qputs(modname);
    } else {
      nputs(modname);
      s::putchar(':');
    }
    for (const auto& d : m->deps) {
      s::putchar(' ');
      if (flags & PRINTMOD_LIST) qputs(d);
      else nputs(d);
    }
  } else if (flags & PRINTMOD_EXIST) {
    if (m->flags & Module::ALIAS) {
      if (not (flags & PRINTMOD_ALIAS) or not (m = find_module(m->alias, FINDMOD_ALIASP))) return;
    }
    if (not m->loaded or (m->flags & Module::UNLOAD)) return;
    nputs(modname);
  } else if (m->flags & Module::ALIAS) {
    if (flags & PRINTMOD_LIST) {
      s::fputs("zmodload -A ", stdout);
      if (modname[0] == '-') s::fputs("-- ", stdout);
      qputs(modname);
      s::putchar('=');
      qputs(m->alias);
    } else {
      nputs(modname);
      s::fputs(" -> ", stdout);
      nputs(m->alias);
    }
  } else if (m->loaded or (flags & PRINTMOD_AUTO)) {
    if (flags & PRINTMOD_LIST) {
      vec<str> features;
      bool with_features = false;
      if (flags & PRINTMOD_AUTO) {
        if (m->autoloads.empty()) return;
      } else if (flags & PRINTMOD_FEATURES) {
        if (m->features.no_features or m->dlhandle) return;
        features = features_of(*m);
        if (features.empty()) return;
        with_features = true;
      }
      s::fputs("zmodload ", stdout);
      if (flags & PRINTMOD_AUTO) s::fputs("-Fa ", stdout);
      else if (with_features) s::fputs("-F ", stdout);
      if (modname[0] == '-') s::fputs("-- ", stdout);
      qputs(modname);
      if (flags & PRINTMOD_AUTO) {
        for (const auto& a : m->autoloads) {
          s::putchar(' ');
          qputs(a);
        }
      } else if (with_features) {
        for (s::size_t i = 0; i < features.size(); ++i) {
          bool on = i < m->enables.size() and m->enables[i];
          if (flags & PRINTMOD_LISTALL) s::printf(" %s", on ? "+" : "-");
          else if (not on) continue;
          else s::putchar(' ');
          qputs(features[i]);
        }
      }
    } else {
      nputs(modname);
    }
  } else {
    return;
  }
  s::putchar('\n');
}

int ModuleManager::zmodload(const vec<str>& argv) {
  ZOps ops;
  int first = zsh_options(argv, "AFRILP:abcfdilmpsue", 0, -1, ops);
  if (first < 0) return 1;
  strv nam = argv[0];
  vec<str> args(argv.begin() + first, argv.end());
  bool ops_bcpf = ops.isset('b') or ops.isset('c') or ops.isset('p') or ops.isset('f');
  bool ops_au   = ops.isset('a') or ops.isset('u');
  if (ops_bcpf and not ops_au) {
    warnnam(nam, "-b, -c, -f, and -p must be combined with -a or -u");
    return 1;
  }
  if (ops.isset('F') and (ops_bcpf or ops.isset('u'))) {
    warnnam(nam, "-b, -c, -f, -p and -u cannot be combined with -F");
    return 1;
  }
  if (ops.isset('A') or ops.isset('R')) {
    if (ops_bcpf or ops_au or ops.isset('d') or (ops.isset('R') and ops.isset('e'))) {
      warnnam(nam, "illegal flags combined with -A or -R");
      return 1;
    }
    if (not ops.isset('e')) return zmodload_alias(nam, args, ops);
  }
  if (ops.isset('d') and ops.isset('a')) {
    warnnam(nam, "-d cannot be combined with -a");
    return 1;
  }
  if (ops.isset('u') and args.empty()) {
    warnnam(nam, "what do you want to unload?");
    return 1;
  }
  if (ops.isset('e')
      and (ops.isset('I') or ops.isset('L') or (ops.isset('a') and not ops.isset('F'))
           or ops.isset('d') or ops.isset('i') or ops.isset('u'))) {
    warnnam(nam, "-e cannot be combined with other options");
    return 1;
  }
  for (char c : {'l', 'P'}) {
    if (ops.isset(c) and not ops.isset('F')) {
      warnnam(nam, str("-") + c + " is only allowed with -F");
      return 1;
    }
  }
  if (ops.isset('F')) return zmodload_features(nam, args, ops);
  if (ops.isset('e')) return zmodload_exist(args, ops);
  if (ops.isset('d')) return zmodload_dep(args, ops);
  int autoopts = ops.isset('b') + ops.isset('c') + ops.isset('p') + ops.isset('f');
  if (autoopts or ops.isset('a')) {
    if (autoopts > 1) {
      warnnam(nam, "use only one of -b, -c, or -p");
      return 1;
    }
    return zmodload_auto(nam, args, ops);
  }
  return zmodload_load(nam, args, ops);
}

int ModuleManager::zmodload_alias(strv nam, const vec<str>& args, const ZOps& ops) {
  if (args.empty()) {
    if (ops.isset('R')) {
      warnnam(nam, "no module alias to remove");
      return 1;
    }
    for (const Module* m : sorted_modules())
      if (m->flags & Module::ALIAS) print_module(*m, ops.isset('L') ? PRINTMOD_LIST : 0);
    return 0;
  }
  for (const str& word : args) {
    auto eq = word.find('=');
    str name = word.substr(0, eq);
    opt<str> aliasname;
    if (eq != str::npos) aliasname = word.substr(eq + 1);
    if (not modname_ok(name)) {
      warnnam(nam, "invalid module name `" + name + "'");
      return 1;
    }
    if (ops.isset('R')) {
      if (aliasname) {
        warnnam(nam, "bad syntax for removing module alias: " + name);
        return 1;
      }
      Module* m = find_module(name, 0);
      if (not m) {
        warnnam(nam, "no such module alias: " + name);
        return 1;
      }
      if (not (m->flags & Module::ALIAS)) {
        warnnam(nam, "module is not an alias: " + name);
        return 1;
      }
      delete_module(name);
    } else if (aliasname) {
      if (not modname_ok(*aliasname)) {
        warnnam(nam, "invalid module name `" + *aliasname + "'");
        return 1;
      }
      str mname = *aliasname;
      for (Module* m = nullptr;;) {
        if (mname == name) {
          warnnam(nam, "module alias would refer to itself: " + name);
          return 1;
        }
        m = find_module(mname, 0);
        if (not m or not (m->flags & Module::ALIAS)) break;
        mname = m->alias;
      }
      Module* m = find_module(name, 0);
      if (m and not (m->flags & Module::ALIAS)) {
        warnnam(nam, "module is not an alias: " + name);
        return 1;
      }
      if (not m) {
        m = find_module(name, FINDMOD_CREATE);
        m->flags = Module::ALIAS;
      }
      m->alias = *aliasname;
    } else {
      Module* m = find_module(name, 0);
      if (not m) {
        warnnam(nam, "no such module alias: " + name);
        return 1;
      }
      if (not (m->flags & Module::ALIAS)) {
        warnnam(nam, "module is not an alias: " + name);
        return 1;
      }
      print_module(*m, ops.isset('L') ? PRINTMOD_LIST : 0);
    }
  }
  return 0;
}

int ModuleManager::zmodload_exist(const vec<str>& args, const ZOps& ops) {
  if (args.empty()) {
    for (const Module* m : sorted_modules())
      print_module(*m, ops.isset('A') ? PRINTMOD_EXIST | PRINTMOD_ALIAS : PRINTMOD_EXIST);
    return 0;
  }
  for (const str& a : args) {
    const Module* m = find_module(a, FINDMOD_ALIASP);
    if (not m or not m->loaded or (m->flags & Module::UNLOAD)) return 1;
  }
  return 0;
}

int ModuleManager::zmodload_dep(const vec<str>& args, const ZOps& ops) {
  if (ops.isset('u')) {
    // Remove dependencies.
    str tnam = args[0];
    Module* m = find_module(tnam, FINDMOD_ALIASP, &tnam);
    if (not m) return 0;
    if (args.size() > 1 and not m->deps.empty()) {
      for (s::size_t i = 1; i < args.size(); ++i) {
        auto at = s::find(m->deps.begin(), m->deps.end(), args[i]);
        if (at != m->deps.end()) m->deps.erase(at);
      }
    } else {
      m->deps.clear();
    }
    if (m->deps.empty() and not m->loaded) delete_module(m->name);
    return 0;
  }
  if (args.size() < 2) {
    int depflags = ops.isset('L') ? PRINTMOD_DEPS | PRINTMOD_LIST : PRINTMOD_DEPS;
    if (not args.empty()) {
      if (const Module* m = find_module(args[0], 0)) print_module(*m, depflags);
    } else {
      for (const Module* m : sorted_modules()) print_module(*m, depflags);
    }
    return 0;
  }
  for (s::size_t i = 1; i < args.size(); ++i) add_dep(args[0], args[i]);
  return 0;
}

int ModuleManager::zmodload_auto(strv nam, const vec<str>& args, const ZOps& ops) {
  char fchar;
  bool list = ops.isset('L');
  if (ops.isset('c')) {
    if (args.empty()) {
      for (const auto& c : mConds) {
        if (c.def) continue;
        if (list)
          s::printf("zmodload -ac%s %s %s\n", c.infix ? "I" : "", c.module.c_str(), c.name.c_str());
        else
          s::printf("%s%s (%s)\n", c.infix ? "infix " : "post ", c.name.c_str(), c.module.c_str());
      }
      return 0;
    }
    fchar = ops.isset('I') ? 'C' : 'c';
  } else if (ops.isset('p')) {
    if (args.empty()) {
      vec<s::pair<str, str>> marks;
      mCtx->params.for_each({}, [&](const Param& p) {
        if (p.flags & PM_AUTOLOAD) marks.emplace_back(p.name, p.scalar());
        return false;
      });
      s::sort(marks.begin(), marks.end());
      for (const auto& [name, mod] : marks) {
        if (list) s::printf("zmodload -ap %s %s\n", mod.c_str(), name.c_str());
        else s::printf("%s (%s)\n", name.c_str(), mod.c_str());
      }
      return 0;
    }
    fchar = 'p';
  } else if (ops.isset('f')) {
    if (args.empty()) {
      for (const auto& f : mMathFuncs.marks()) {
        if (list) s::printf("zmodload -af %s %s\n", f.module.c_str(), f.name.c_str());
        else s::printf("%s (%s)\n", f.name.c_str(), f.module.c_str());
      }
      return 0;
    }
    fchar = 'f';
  } else {
    if (args.empty()) {
      // zsh's autoloadscan over the sorted builtin table.
      vec<s::pair<str, str>> marks;
      for (const auto& [name, e] : mBuiltins)
        if (not e.def) marks.emplace_back(name, e.module);
      s::sort(marks.begin(), marks.end());
      for (const auto& [name, mod] : marks) {
        if (list) {
          s::fputs("zmodload -ab ", stdout);
          if (not mod.empty() and mod[0] == '-') s::fputs("-- ", stdout);
          qputs(mod);
          if (name != mod) {
            s::putchar(' ');
            qputs(name);
          }
        } else {
          nputs(name);
          if (name != mod) {
            s::fputs(" (", stdout);
            nputs(mod);
            s::putchar(')');
          }
        }
        s::putchar('\n');
      }
      return 0;
    }
    fchar = 'b';
  }
  int flags = FEAT_AUTOALL;
  if (ops.isset('i')) flags |= FEAT_IGNORE;
  if (ops.isset('u')) {
    flags |= FEAT_REMOVE;
    return autofeatures(nam, nullptr, args, fchar, flags);
  }
  str modnam = args[0];
  vec<str> rest(args.size() > 1 ? args.begin() + 1 : args.begin(), args.end());
  return autofeatures(nam, &modnam, rest, fchar, flags);
}

int ModuleManager::zmodload_load(strv nam, const vec<str>& args, const ZOps& ops) {
  if (ops.isset('u')) {
    int ret = 0;
    for (const str& a : args)
      if (unload_named_module(a, nam, ops.isset('i'))) ret = 1;
    return ret;
  }
  if (args.empty()) {
    for (const Module* m : sorted_modules())
      if (not (m->flags & (Module::UNLOAD | Module::ALIAS)))
        print_module(*m, ops.isset('L') ? PRINTMOD_LIST : 0);
    return 0;
  }
  int ret = 0;
  for (const str& a : args) {
    int tmp = require_module(a, nullptr, ops.isset('s'));
    if (tmp and ret != 1) ret = tmp;
    if (mCtx and mCtx->aborting) break;
  }
  return ret;
}

int ModuleManager::zmodload_features(strv nam, const vec<str>& args_in, const ZOps& ops) {
  vec<str> args = args_in;
  str modname;
  if (not args.empty()) {
    modname = args.front();
    args.erase(args.begin());
  } else if (ops.isset('L')) {
    if (ops.isset('P')) {
      warnnam(nam, "-P is only allowed with a module name");
      return 1;
    }
    int printflags = PRINTMOD_LIST | PRINTMOD_FEATURES;
    if (ops.isset('l')) printflags |= PRINTMOD_LISTALL;
    if (ops.isset('a')) printflags |= PRINTMOD_AUTO;
    for (const Module* m : sorted_modules())
      if (not (m->flags & Module::ALIAS)) print_module(*m, printflags);
    return 0;
  }
  if (args_in.empty()) {
    warnnam(nam, "-F requires a module name");
    return 1;
  }
  vec<opt<CompiledPattern>> pats;
  if (ops.isset('m')) {
    bool extended = mCtx and mCtx->opts.get(OPT_EXTENDEDGLOB);
    for (const str& a : args) {
      strv arg = a;
      if (not arg.empty() and (arg[0] == '+' or arg[0] == '-')) arg.remove_prefix(1);
      pats.push_back(CompiledPattern::compile(arg, false, extended));
    }
  }
  auto matches = [&](s::size_t i, const str& feature) {
    strv arg = args[i];
    if (not pats.empty()) return pats[i]->matches(feature);
    return arg == feature;
  };
  // As matches(), +/- dropped.
  auto matches_bare = [&](s::size_t i, const str& feature) {
    strv arg = args[i];
    if (not arg.empty() and (arg[0] == '+' or arg[0] == '-')) arg.remove_prefix(1);
    if (not pats.empty()) return pats[i]->matches(feature);
    return arg == feature;
  };
  if (ops.isset('l') or ops.isset('L') or ops.isset('e')) {
    const str* param = ops.arg_safe('P');
    Module* m = find_module(modname, FINDMOD_ALIASP);
    if (ops.isset('a')) {
      if (not m or not m->has_autoloads) return 1;
      if (ops.isset('e')) {
        for (const str& a : args) {
          strv f = a;
          bool sense = true;
          if (not f.empty() and f[0] == '+') {
            f.remove_prefix(1);
          } else if (not f.empty() and f[0] == '-') {
            f.remove_prefix(1);
            sense = false;
          }
          if (contains(m->autoloads, f) != sense) return 1;
        }
        return 0;
      }
      if (param) {
        auto r = mCtx->params.set_array(*param, m->autoloads);
        if (not r) {
          warn(r.error().message);
          return 1;
        }
        return 0;
      }
      if (ops.isset('L'))
        s::printf("zmodload -aF %s%c", m->name.c_str(), m->autoloads.empty() ? '\n' : ' ');
      for (s::size_t i = 0; i < m->autoloads.size(); ++i)
        s::printf("%s%c", m->autoloads[i].c_str(),
                  ops.isset('L') and i + 1 < m->autoloads.size() ? ' ' : '\n');
      return 0;
    }
    if (not m or not m->loaded or (m->flags & Module::UNLOAD)) {
      if (not ops.isset('e')) warnnam(nam, "module `" + modname + "' is not yet loaded");
      return 1;
    }
    if (m->features.no_features or m->dlhandle) {
      if (not ops.isset('e')) warnnam(nam, "module `" + m->name + "' does not support features");
      return 1;
    }
    vec<str> features = features_of(*m);
    const vec<s::uint8_t>& enables = m->enables;
    for (s::size_t i = 0; i < args.size(); ++i) {
      strv arg = args[i];
      int on = -1;
      if (not arg.empty() and arg[0] == '-') {
        on = 0;
        arg.remove_prefix(1);
      } else if (not arg.empty() and arg[0] == '+') {
        on = 1;
        arg.remove_prefix(1);
      }
      int found = 0;
      for (s::size_t k = 0; k < features.size(); ++k) {
        if (not pats.empty() ? pats[i]->matches(features[k]) : arg == features[k]) {
          if (ops.isset('e') and on != -1 and on != (enables[k] & 1)) return 1;
          ++found;
          if (pats.empty()) break;
        }
      }
      if (not found) {
        if (not ops.isset('e'))
          warnnam(nam, "module `" + modname + "' has no "
                           + (pats.empty() ? "such feature" : "feature matching") + ": `"
                           + args[i] + "'");
        return 1;
      }
    }
    if (ops.isset('e')) return 0;
    bool only_on = ops.isset('L') and not ops.isset('l');
    // zsh selects with the +/- dropped when it counts, but compares the
    // whole word when it prints.
    auto selected = [&](const str& feature, bool bare) {
      if (args.empty()) return true;
      for (s::size_t i = 0; i < args.size(); ++i)
        if (bare ? matches_bare(i, feature) : matches(i, feature)) return true;
      return false;
    };
    if (param) {
      vec<str> arr;
      for (s::size_t k = 0; k < features.size(); ++k) {
        if (only_on and not enables[k]) continue;
        if (not selected(features[k], true)) continue;
        if (not selected(features[k], false)) continue;
        arr.push_back((only_on ? "" : enables[k] ? "+" : "-") + features[k]);
      }
      auto r = mCtx->params.set_array(*param, s::move(arr));
      if (not r) {
        warn(r.error().message);
        return 1;
      }
      return 0;
    }
    if (ops.isset('L')) s::printf("zmodload -F %s ", m->name.c_str());
    for (s::size_t k = 0; k < features.size(); ++k) {
      if (not selected(features[k], false)) continue;
      const char* onoff;
      if (only_on) {
        if (not enables[k]) continue;
        onoff = "";
      } else {
        onoff = enables[k] ? "+" : "-";
      }
      s::printf("%s%s%c", onoff, features[k].c_str(),
                ops.isset('L') and k + 1 < features.size() ? ' ' : '\n');
    }
    return 0;
  }
  if (ops.isset('P')) {
    warnnam(nam, "-P can only be used with -l or -L");
    return 1;
  }
  if (ops.isset('a')) {
    if (ops.isset('m')) {
      warnnam(nam, "-m cannot be used with -a");
      return 1;
    }
    return autofeatures(nam, &modname, args, 0, FEAT_IGNORE);
  }
  vec<FeatureEnable> features;
  for (s::size_t i = 0; i < args.size(); ++i)
    features.push_back({args[i], pats.empty() ? s::nullopt : pats[i]});
  return require_module(modname, &features, ops.isset('s'));
}

int builtin_zmodload(vec<str>& args, ExecContext& ctx) { return ctx.modules.zmodload(args); }

}  // namespace jfsh
