#ifndef JFSH_CORE_PARAM_H
#define JFSH_CORE_PARAM_H
// jfsh/core/param.h — shell parameters (variables).
//
// Mirrors zsh's Param struct and params.c.  Every shell variable — scalars,
// integers, floats, arrays, associative arrays — is a Param.  Special params
// ($?, $$, $!, $-, etc.) attach custom get/set/unset handlers.

#include <cstdint>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

enum ParamFlags : s::uint32_t {
  PM_SCALAR    = 0x0000'0001,
  PM_INTEGER   = 0x0000'0002,
  PM_FLOAT     = 0x0000'0004,
  PM_ARRAY     = 0x0000'0008,
  PM_HASH      = 0x0000'0010,
  PM_SPECIAL   = 0x0000'0020,
  PM_READONLY  = 0x0000'0040,
  PM_EXPORTED  = 0x0000'0080,
  PM_LOCAL     = 0x0000'0100,
  PM_UNIQUE    = 0x0000'0200,
  PM_TIED      = 0x0000'0400,
  PM_UPPER     = 0x0000'0800,
  PM_LOWER     = 0x0000'1000,
  PM_HIDE      = 0x0000'2000,
  PM_UNSET     = 0x0000'4000,
  PM_EFLOAT    = 0x0000'8000,   // float shown in exponent form (typeset -E)
  // zsh/param/private (M11): visible only in the scope that declared it, not to
  // the functions that scope calls.
  PM_PRIVATE   = 0x0001'0000,
  PM_HIDEVAL   = 0x0002'0000,   // typeset -H: listings show the name only
};

using ParamValue = s::variant<
  str,
  s::int64_t,
  double,
  vec<str>,
  umap<str, str>
>;

struct ParamHandlers {
  func<str()>                     get_scalar;
  func<void(str)>                 set_scalar;
  func<vec<str>()>                get_array;
  func<void(vec<str>)>            set_array;
  func<umap<str, str>()>          get_hash;   // M10 Phase 2: live hash views
  func<void(umap<str, str>)>      set_hash;
  func<void()>                    unset_fn;
  // M11: per-key access to a special associative array ($mapfile,
  // $terminfo, a ztie'd database, …), so `${h[key]}`, `h[key]=v` and
  // `unset 'h[key]'` touch one element instead of the whole table.
  func<opt<str>(const str& key)>          get_elem;
  func<void(const str& key, str value)>   set_elem;
  func<void(const str& key)>              unset_elem;
  // Keys that cannot be assigned (`$compstate[nmatches]`).
  func<bool(const str& key)>              readonly_elem;
};

struct Param {
  str         name;
  ParamValue  value;
  s::uint32_t flags    = PM_SCALAR | PM_UNSET;
  int         scope    = 0;

  // nullptr for ordinary parameters
  uptr<ParamHandlers> handlers;
  // A special parameter made local: the value it had, given back through
  // the handlers when the scope ends.
  opt<ParamValue> restore;

  bool is_set()      const noexcept { return not (flags & PM_UNSET); }
  bool is_exported() const noexcept { return flags & PM_EXPORTED; }
  bool is_readonly() const noexcept { return flags & PM_READONLY; }
  bool is_array()    const noexcept { return flags & PM_ARRAY; }
  bool is_hash()     const noexcept { return flags & PM_HASH; }
  bool is_integer()  const noexcept { return flags & PM_INTEGER; }
  bool is_float()    const noexcept { return flags & PM_FLOAT; }
  bool is_scalar()   const noexcept { return flags & PM_SCALAR; }

  // An integer or float renders as text: floats as `typeset -F` (10
  // decimals) or, with PM_EFLOAT, `typeset -E` (10 significant digits).
  str             scalar()  const noexcept;
  s::int64_t      integer() const noexcept;
  double          floatv()  const noexcept;
  vec<str>        array()   const noexcept;
  umap<str, str>  hash()    const noexcept;
  // The stored array or association itself, for reading one element
  // without copying the rest; empty for a value of another type.
  const vec<str>&       array_ref() const noexcept;
  const umap<str, str>& hash_ref()  const noexcept;
  // The stored array moved out, for a caller that sets it again; an
  // empty array for a value of another type.
  vec<str>              take_array() noexcept;
  // The stored association, to change one element in place; nullptr for
  // a value of another type.
  umap<str, str>*       hash_ptr() noexcept { return s::get_if<umap<str, str>>(&value); }

  void set(str s) noexcept {
    value = s::move(s);
    flags = (flags & ~(PM_UNSET | PM_INTEGER | PM_FLOAT | PM_ARRAY | PM_HASH))
          | PM_SCALAR;
  }
  void set(s::int64_t i) noexcept {
    value = i;
    flags = (flags & ~(PM_UNSET | PM_SCALAR | PM_FLOAT | PM_ARRAY | PM_HASH))
          | PM_INTEGER;
  }
  void set(double f) noexcept {
    value = f;
    flags = (flags & ~(PM_UNSET | PM_SCALAR | PM_INTEGER | PM_ARRAY | PM_HASH))
          | PM_FLOAT;
  }
  void set(vec<str> a) noexcept {
    value = s::move(a);
    flags = (flags & ~(PM_UNSET | PM_SCALAR | PM_INTEGER | PM_FLOAT | PM_HASH))
          | PM_ARRAY;
  }
  void set(umap<str, str> h) noexcept {
    value = s::move(h);
    flags = (flags & ~(PM_UNSET | PM_SCALAR | PM_INTEGER | PM_FLOAT | PM_ARRAY))
          | PM_HASH;
  }
};

struct ParamTable {
  // Look up by name, searching inner → outer scopes.
  optref<Param> lookup(strv name) noexcept;

  optref<const Param> lookup(strv name) const noexcept;

  // As lookup(), without running a special parameter's getter: for a
  // reader that wants the handlers, not the value.
  optref<Param> lookup_raw(strv name) noexcept;

  // The parameter in the innermost scope only (for `local`), or in a
  // command's prefix scope above it.
  optref<Param> lookup_local(strv name) noexcept;

  // Create or overwrite in the current scope.
  Param& assign(
    str name,
    ParamValue value,
    s::uint32_t flags = 0);

  // Shell assignment (`name=value`, `name+=value`): updates the innermost
  // existing parameter in place, keeping its attributes (e.g. PM_EXPORTED),
  // or creates a new global.  Fails on a readonly parameter.
  Result<void> set_scalar(
    strv name,
    str value,
    bool append = false);

  // Array assignment (`name=(…)`, `name+=(…)`), with set_scalar's scoping
  // and readonly rules.  An existing associative array takes the words as
  // key/value pairs.
  Result<void> set_array(
    strv name,
    vec<str> words,
    bool append = false);

  // Declare local — creates in the innermost scope that isn't a command's
  // prefix scope, without setting.
  Param& declare_local(
    str name,
    s::uint32_t flags = PM_SCALAR | PM_UNSET);

  // `unset 'name[key]'`: removes one key of an associative array (through
  // its handlers' unset_elem when it has one).  Fails on a readonly one.
  Result<void> unset_element(strv name, const str& key);

  // `unset name`: removes the innermost parameter of that name; a local
  // one stays, unset, so the outer one doesn't show through.  Fails on a
  // readonly parameter; a name that isn't set is fine.
  Result<void> unset(strv name);

  // typeset -T: ties scalar `scalar` (in the scope it lives in) to the
  // array `array`, which holds the value: reading the scalar joins the
  // array with `sep`, assigning it splits the text into the array.  Both
  // get PM_TIED.
  void tie(Param& scalar, str array, char sep);

  // A `prefix` scope holds the assignments in front of one command:
  // `local`/`typeset` of a new name declare below it.
  void push_scope(bool prefix = false);
  void pop_scope();

  vec<str> make_envp() const;

  void for_each(func<void(const Param&)>) const;

  int current_scope() const noexcept {
    return static_cast<int>(mScopes.size()) - 1;
  }
  // The innermost scope that isn't a prefix scope.
  int local_scope() const noexcept;
  // Whether `p` lives in the innermost scope.
  bool in_current_scope(const Param& p) const noexcept;
  // Whether `p` lives in a command's prefix scope.
  bool in_prefix_scope(const Param& p) const noexcept;

  // Counts assignments to $PATH (and scopes ending with one), even those
  // that keep its text, so the command hash can be dropped on each, as zsh
  // does.
  s::uint64_t path_generation() const noexcept { return mPathGen; }
  // For a caller that stores into a Param itself (typeset): counts the
  // assignment to `name`.
  void note_assigned(strv name) noexcept {
    if (name == "PATH" or name == "path") ++mPathGen;
  }

private:

  // mScopes[0] = global; mScopes.back() = innermost local
  vec<HashTable<Param>> mScopes;
  vec<int>              mPrefixScopes;   // indexes of prefix scopes, ascending
  s::uint64_t           mPathGen = 0;
};

// The type text ${(t)name} and $parameters give: "scalar", "array-local",
// "scalar-tied-export-special", …
str param_type_text(const Param& p);

}  // namespace jfsh

#endif  // JFSH_CORE_PARAM_H
