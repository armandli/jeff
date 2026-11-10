#pragma once
// jfsh/core/param.hh — shell parameters (variables).
//
// Mirrors zsh's Param struct and params.c.  Every shell variable — scalars,
// integers, floats, arrays, associative arrays — is a Param.  Special params
// ($?, $$, $!, $-, etc.) attach custom get/set/unset handlers.

#include <jfsh/core/types.hh>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace jfsh {

// ---------------------------------------------------------------------------
// Param flags (mirrors zsh PM_* constants)
// ---------------------------------------------------------------------------
enum ParamFlags : std::uint32_t {
    PM_SCALAR    = 0x0000'0001,
    PM_INTEGER   = 0x0000'0002,
    PM_FLOAT     = 0x0000'0004,
    PM_ARRAY     = 0x0000'0008,
    PM_HASH      = 0x0000'0010,
    PM_SPECIAL   = 0x0000'0020,     // special parameter with custom handlers
    PM_READONLY  = 0x0000'0040,
    PM_EXPORTED  = 0x0000'0080,     // in environment
    PM_LOCAL     = 0x0000'0100,     // local to function
    PM_UNIQUE    = 0x0000'0200,     // array: remove duplicates
    PM_TIED      = 0x0000'0400,     // array/scalar pair tied together (typeset -T)
    PM_UPPER     = 0x0000'0800,     // string: auto-uppercase
    PM_LOWER     = 0x0000'1000,     // string: auto-lowercase
    PM_HIDE      = 0x0000'2000,     // hidden from param list
    PM_UNSET     = 0x0000'4000,     // currently unset (but declared)
};

// ---------------------------------------------------------------------------
// Param value — the five primitive storage types zsh supports
// ---------------------------------------------------------------------------
using ParamValue = std::variant<
    std::string,                                        // PM_SCALAR
    long long,                                          // PM_INTEGER
    double,                                             // PM_FLOAT
    std::vector<std::string>,                           // PM_ARRAY
    std::unordered_map<std::string, std::string>        // PM_HASH
>;

// ---------------------------------------------------------------------------
// Special parameter handler set — installed on built-in params like $PATH
// ---------------------------------------------------------------------------
struct ParamHandlers {
    std::function<std::string()>                     get_scalar;
    std::function<void(std::string)>                 set_scalar;
    std::function<std::vector<std::string>()>        get_array;
    std::function<void(std::vector<std::string>)>    set_array;
    std::function<void()>                            unset_fn;
};

// ---------------------------------------------------------------------------
// Param — one shell variable
// ---------------------------------------------------------------------------
struct Param {
    std::string   name;
    ParamValue    value;
    std::uint32_t flags    = PM_SCALAR | PM_UNSET;
    int           scope    = 0;     // nesting depth (0 = global)

    // Optional special handlers (nullptr for ordinary parameters)
    std::unique_ptr<ParamHandlers> handlers;

    bool is_set()      const noexcept { return !(flags & PM_UNSET); }
    bool is_exported() const noexcept { return   flags & PM_EXPORTED; }
    bool is_readonly() const noexcept { return   flags & PM_READONLY; }
    bool is_array()    const noexcept { return   flags & PM_ARRAY; }
    bool is_hash()     const noexcept { return   flags & PM_HASH; }
    bool is_integer()  const noexcept { return   flags & PM_INTEGER; }
    bool is_float()    const noexcept { return   flags & PM_FLOAT; }
    bool is_scalar()   const noexcept { return   flags & PM_SCALAR; }

    // Convenience accessors — return empty/0 if wrong type or unset
    std::string               scalar()  const noexcept;
    long long                 integer() const noexcept;
    double                    floatv()  const noexcept;
    std::vector<std::string>  array()   const noexcept;
    std::unordered_map<std::string, std::string> hash() const noexcept;

    // Set helpers
    void set(std::string s)                              noexcept;
    void set(long long   i)                              noexcept;
    void set(double      f)                              noexcept;
    void set(std::vector<std::string> a)                 noexcept;
    void set(std::unordered_map<std::string,std::string> h) noexcept;
};

// ---------------------------------------------------------------------------
// ParamTable — the shell's variable store for one scope level.
// Global scope is scope 0; each function call pushes a new local scope.
// ---------------------------------------------------------------------------
class ParamTable {
public:
    // Look up a parameter by name, searching inner → outer scopes.
    Param*       lookup(std::string_view name)        noexcept;
    const Param* lookup(std::string_view name)  const noexcept;

    // Create or overwrite in the current scope.
    Param& assign(std::string name, ParamValue value, std::uint32_t flags = 0);

    // Declare local (creates in current scope without setting).
    Param& declare_local(std::string name, std::uint32_t flags = PM_SCALAR | PM_UNSET);

    // Push a new function-local scope.
    void push_scope();
    // Pop the innermost scope; frees locals, restores overridden globals.
    void pop_scope();

    // Collect all exported params for exec*() environment.
    std::vector<std::string> make_envp() const;

    // Enumerate all visible params (for typeset -p / declare -p).
    void for_each(std::function<void(const Param&)>) const;

    int current_scope() const noexcept { return static_cast<int>(scopes_.size()) - 1; }

private:
    // scopes_[0] = global; scopes_.back() = innermost local
    std::vector<HashTable<Param>> scopes_;
};

} // namespace jfsh
