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
#include <variant>
#include <vector>

#include <jfsh/core/types.h>

namespace jfsh {

enum ParamFlags : std::uint32_t {
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
};

using ParamValue = std::variant<
  std::string,
  long long,
  double,
  std::vector<std::string>,
  std::unordered_map<std::string, std::string>
>;

struct ParamHandlers {
  std::function<std::string()>                  get_scalar;
  std::function<void(std::string)>              set_scalar;
  std::function<std::vector<std::string>()>     get_array;
  std::function<void(std::vector<std::string>)> set_array;
  std::function<void()>                         unset_fn;
};

struct Param {
  std::string   name;
  ParamValue    value;
  std::uint32_t flags    = PM_SCALAR | PM_UNSET;
  int           scope    = 0;

  // nullptr for ordinary parameters
  std::unique_ptr<ParamHandlers> handlers;

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
  std::string                                   scalar()  const noexcept;
  long long                                     integer() const noexcept;
  double                                        floatv()  const noexcept;
  std::vector<std::string>                      array()   const noexcept;
  std::unordered_map<std::string, std::string>  hash()    const noexcept;

  void set(std::string s)                                     noexcept;
  void set(long long   i)                                     noexcept;
  void set(double      f)                                     noexcept;
  void set(std::vector<std::string> a)                        noexcept;
  void set(std::unordered_map<std::string, std::string> h)    noexcept;
};

struct ParamTable {
  // Look up by name, searching inner → outer scopes.
  std::optional<std::reference_wrapper<Param>>
  lookup(std::string_view name) noexcept;

  std::optional<std::reference_wrapper<const Param>>
  lookup(std::string_view name) const noexcept;

  // The parameter in the innermost scope only (for `local`).
  std::optional<std::reference_wrapper<Param>>
  lookup_local(std::string_view name) noexcept;

  // Create or overwrite in the current scope.
  Param& assign(
    std::string name,
    ParamValue value,
    std::uint32_t flags = 0);

  // Shell assignment (`name=value`, `name+=value`): updates the innermost
  // existing parameter in place, keeping its attributes (e.g. PM_EXPORTED),
  // or creates a new global.  Fails on a readonly parameter.
  Result<void> set_scalar(
    std::string_view name,
    std::string value,
    bool append = false);

  // Array assignment (`name=(…)`, `name+=(…)`), with set_scalar's scoping
  // and readonly rules.  An existing associative array takes the words as
  // key/value pairs.
  Result<void> set_array(
    std::string_view name,
    std::vector<std::string> words,
    bool append = false);

  // Declare local — creates in current scope without setting.
  Param& declare_local(
    std::string name,
    std::uint32_t flags = PM_SCALAR | PM_UNSET);

  void push_scope();
  void pop_scope();

  std::vector<std::string> make_envp() const;

  void for_each(std::function<void(const Param&)>) const;

  int current_scope() const noexcept {
    return static_cast<int>(mScopes.size()) - 1;
  }

private:
  // mScopes[0] = global; mScopes.back() = innermost local
  std::vector<HashTable<Param>> mScopes;
};

}  // namespace jfsh

#endif  // JFSH_CORE_PARAM_H
