#ifndef JFSH_MODULES_MODULE_H
#define JFSH_MODULES_MODULE_H
// jfsh/modules/module.h — loadable and compiled-in module interface (M10).
//
// A port of zsh's Src/module.c.  A module contributes features to the
// shell: builtins (`b:name`), conditions (`c:name` prefix, `C:name` infix),
// math functions (`f:name`) and parameters (`p:name`).  Each feature is on
// or off (`zmodload -F`), and a feature of a module not loaded yet can be
// marked for autoload: the first use loads the module (`zmodload -a`,
// `zmodload -Fa`).
//
// Modules are compiled in: `register_builtin_module()` hands the manager a
// callback that fills `ModuleFeatures` when the module is loaded (zsh's
// linked modules).  A module not compiled in is looked for in
// `$module_path` as `NAME.so` and dlopen'd; such a module has `setup_`,
// `boot_`, `cleanup_` and `finish_` entry points and no features.
//
// The user-facing surface is the `zmodload` builtin, a port of
// bin_zmodload (src/modules/module.cpp).

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
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct ExecContext;  // forward — defined in jfsh/exec/executor.h

// A math function contributed by a module (zsh/mathfunc, zsh/example …).
// Returns the function's result, or a string error.  `args` is the fully
// evaluated argument list.  A string function (zsh's MFF_STR) takes its
// argument text unevaluated instead, through `str_fn`.
using MathFunc    = func<Result<MathNum>(const vec<MathNum>&)>;
using MathStrFunc = func<Result<MathNum>(strv)>;

struct MathFuncDef {
  str         name;
  MathFunc    fn;
  MathStrFunc str_fn;        // set for a string function
  int         min_args = -1; // -1 = any
  int         max_args = -1;
};

// The math functions by name: those modules added and the autoload marks
// of those not loaded yet (zsh's mathfuncs list).  The arithmetic evaluator
// (math.cpp) looks up a bare identifier followed by `(` here; finding a
// mark loads its module first.
struct MathFuncRegistry {
  // Marks: name → module, newest first as zsh lists them.
  struct Mark { str name, module; bool autoall = false; };

  void add(MathFuncDef def) { mFuncs[def.name] = s::move(def); }
  void remove(strv name)    { mFuncs.erase(str(name)); }
  // As zsh's getmathfunc(name, 1): a mark loads its module (and goes).
  // nullptr for no such function; an error when the module that came
  // didn't define it.
  Result<const MathFuncDef*> find(strv name) const;
  // A function defined now (a mark doesn't count).
  bool has(strv name) const { return mFuncs.contains(str(name)); }
  bool empty() const noexcept { return mFuncs.empty(); }

  vec<Mark>& marks() noexcept { return mMarks; }
  const vec<Mark>& marks() const noexcept { return mMarks; }
  // Loads `module` for the mark of `name` (set by ModuleManager).
  func<void(const Mark&)> autoload;

private:
  HashTable<MathFuncDef> mFuncs;
  // find() takes a mark away as it loads the module, as zsh does.
  mutable vec<Mark>      mMarks;
};

// A condition contributed by a module.  An infix one is `[[ lhs -NAME rhs ]]`
// (zsh's CONDF_INFIX, feature `C:NAME`), a prefix one `[[ -NAME args… ]]`
// (feature `c:NAME`).  Each returns 0 for true, 1 for false, 2 for an error.
using CondBinaryFn = func<int(const str& lhs, const str& rhs, ExecContext&)>;
using CondPrefixFn = func<int(const vec<str>& args, ExecContext&)>;

struct CondDef {
  str          name;          // without the `-`: "pcre-match"
  CondBinaryFn fn;            // infix
  CondPrefixFn prefix_fn;     // prefix
  int          min_args = 1;  // prefix: the arguments it takes
  int          max_args = 1;
  bool         infix    = true;
};

// Everything a module can contribute.  Its features, in zsh's order, are
// the builtins, the conditions, the math functions and the parameters.
struct ModuleFeatures {
  vec<BuiltinDef>   builtins;
  vec<CondDef>      conds;
  vec<MathFuncDef>  mathfuncs;
  // The parameters the register callback installed, by name.
  vec<str>          params;
  // Runs when the module is unloaded (`zmodload -u`): drops the hooks and
  // widgets the register callback installed.  The parameters in `params`
  // are removed by the manager.
  func<void()>      on_unload;
  // zsh/main: a module without features (`zmodload -F` says so).
  bool              no_features = false;
};

// One module table entry, zsh's struct module: a module loaded or not, an
// alias for another, or a name that only has dependencies or autoloads.
struct Module {
  enum Flag : s::uint16_t {
    LINKED = 1 << 0,   // compiled in
    ALIAS  = 1 << 1,   // `zmodload -A name=other`
    UNLOAD = 1 << 2,   // unloaded but still in use
    BUSY   = 1 << 3,   // loading its dependencies
    SETUP  = 1 << 4,   // being set up
    INIT_S = 1 << 5,   // set up
    INIT_B = 1 << 6,   // booted
  };
  // A compiled-in module's register callback.  Called when the module is
  // loaded; writes the module's features into `out` (installing its
  // parameters).
  using RegisterFn = func<void(ModuleFeatures& out, ExecContext& ctx)>;

  str                 name;
  str                 alias;       // ALIAS: the module it names
  vec<str>            deps;        // modules loaded first (`zmodload -d`)
  vec<str>            autoloads;   // features marked for autoload, sorted
  ModuleFeatures      features;    // while loaded
  vec<s::uint8_t>     enables;     // one per feature, while loaded
  HashTable<Param>    stashed;     // parameters of features turned off
  void*               dlhandle = nullptr;
  s::uint16_t         flags    = 0;
  bool                loaded   = false;   // zsh's u.handle / u.linked
  // An autoload was ever marked: zsh's m->autoloads list exists (it may
  // be empty again).
  bool                has_autoloads = false;
};

struct FeatureEnable;  // a `+feature` / `-feature` / pattern (module.cpp)
struct ZOps;           // jfsh/builtins/util.h

struct ModuleManager {
  ModuleManager();
  ~ModuleManager();
  ModuleManager(const ModuleManager&) = delete;
  ModuleManager& operator=(const ModuleManager&) = delete;

  // Gives the manager the shell it loads modules into, and how to tell a
  // core builtin (one no module may replace).  The Executor calls it.
  void attach(ExecContext& ctx, func<bool(strv)> core_builtin);

  // Register a compiled-in module (zsh's register_module).  The module is
  // not loaded until it is required.
  void register_builtin_module(str name, Module::RegisterFn fn);

  // zsh's require_module: loads `name` (resolving aliases) with all its
  // features, or makes sure it is loaded.  0 on success, 1 when loading
  // failed, 2 when some features couldn't be set.
  int require(strv name, bool silent = false);
  // zsh's ensurefeature: loads `module` for one feature (`b:` + `name`), or
  // for all when `name` is null.
  int ensure_feature(strv module, strv prefix, const str* name);
  // zsh's module_loaded.
  bool is_loaded(strv name) const;
  // `zmodload -ab`-style marks made at startup, as zsh's bltinmods list:
  // features (`b:x`, `p:y` …) of `module`, and dependencies.
  void autoload_features(strv module, const vec<str>& features);
  void add_dep(strv name, strv from);

  // The builtins modules contribute.  find_builtin gives an added one,
  // autoload_for_builtin the module of a mark (an autoloadable builtin not
  // loaded yet), has_builtin whether there is either; resolve_builtin
  // gives an added one, or loads a mark's module and gives the builtin then
  // defined, printing zsh's error when there is none (`*marked` says there
  // was a mark, so a nullptr is a failure, not "no such builtin").
  const BuiltinDef* find_builtin(strv name) const;
  opt<str>          autoload_for_builtin(strv name) const;
  bool              has_builtin(strv name) const;
  const BuiltinDef* resolve_builtin(strv name, bool* marked = nullptr);
  // Every module builtin and mark, for $builtins, enable/disable, whence -m.
  vec<str>          builtin_names() const;

  // The condition `-name` (infix or prefix), loading a mark's module first
  // (zsh's getconddef(…, 1)); nullptr when none.
  const CondDef* find_cond(strv name, bool infix);

  const MathFuncRegistry& math_funcs() const noexcept { return mMathFuncs; }

  // The modules as $modules shows them: name → "loaded", "autoloaded" or
  // "alias:other".
  vec<s::pair<str, str>> module_states() const;

  // `zmodload` (bin_zmodload): `args[0]` is the command name.
  int zmodload(const vec<str>& args);

protected:
  struct BuiltinEntry {
    str               module;          // the mark's module, or the owner
    const BuiltinDef* def = nullptr;   // null for a mark
    bool              autoall = false;
  };
  struct CondEntry {
    str            name;
    str            module;
    const CondDef* def = nullptr;
    bool           infix = false;
    bool           autoall = false;
  };

  Module* find_module(strv name, int flags, str* namep = nullptr);
  const Module* find_module(strv name, int flags, str* namep = nullptr) const;
  void delete_module(strv name);
  int  load_module(str name, const vec<FeatureEnable>* features, bool silent);
  int  require_module(str name, vec<FeatureEnable>* features, bool silent);
  int  do_module_features(Module& m, vec<FeatureEnable>* features, int flags);
  int  do_boot_module(Module& m, vec<FeatureEnable>* features, bool silent);
  bool setup_module(Module& m, bool silent);
  void finish_module(Module& m);
  int  unload_module(Module& m);
  int  unload_named_module(const str& name, strv cmd, bool silent);
  vec<str> features_of(const Module& m) const;
  bool set_enables(Module& m, const vec<s::uint8_t>& want);
  int  autofeatures(strv cmd, const str* module, const vec<str>& features,
                    char prefchar, int flags);
  int  add_autobin(const str& module, const str& name, int flags);
  int  del_autobin(const str& name, int flags);
  int  add_autocond(const str& module, const str& name, int flags);
  int  del_autocond(const str& name, int flags);
  int  add_automathfunc(const str& module, const str& name, int flags);
  int  del_automathfunc(const str& name, int flags);
  int  add_autoparam(const str& module, const str& name, int flags);
  int  del_autoparam(const str& name, int flags);
  void print_module(const Module& m, int flags) const;
  vec<const Module*> sorted_modules() const;
  void* try_load_module(const str& name, str& err) const;

  int zmodload_alias(strv nam, const vec<str>& args, const ZOps& ops);
  int zmodload_exist(const vec<str>& args, const ZOps& ops);
  int zmodload_dep(const vec<str>& args, const ZOps& ops);
  int zmodload_auto(strv nam, const vec<str>& args, const ZOps& ops);
  int zmodload_load(strv nam, const vec<str>& args, const ZOps& ops);
  int zmodload_features(strv nam, const vec<str>& args, const ZOps& ops);

private:
  HashTable<uptr<Module>>       mModules;
  HashTable<Module::RegisterFn> mLinked;     // compiled-in modules
  HashTable<BuiltinEntry>       mBuiltins;   // module builtins and marks
  vec<CondEntry>                mConds;      // newest first, as condtab
  MathFuncRegistry              mMathFuncs;
  func<bool(strv)>              mCoreBuiltin;
  ExecContext*                  mCtx = nullptr;
};

// Registers every module jfsh ships, makes zsh's startup autoload marks and
// dependencies (its bltinmods list) and loads zsh/main.  main() and the test
// fixture call it.
void setup_shipped_modules(ExecContext& ctx);

}  // namespace jfsh

#endif  // JFSH_MODULES_MODULE_H
