#ifndef JFSH_CORE_ALIASES_H
#define JFSH_CORE_ALIASES_H
// jfsh/core/aliases.h — short names for the standard library, shared by
// all of jfsh.  The aliases live in namespace jfsh, so code outside it
// (main(), the pty tests) still spells std:: out.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jfsh {

namespace s = std;

using str  = s::string;
using strv = s::string_view;

template<typename T, typename A = s::allocator<T>>
using vec = s::vector<T, A>;

template<typename T>
using opt = s::optional<T>;

// A lookup result that refers into a table instead of copying out of it.
template<typename T>
using optref = s::optional<s::reference_wrapper<T>>;

template<typename... Ts>
using umap = s::unordered_map<Ts...>;

template<typename... Ts>
using uset = s::unordered_set<Ts...>;

template<typename T, typename D = s::default_delete<T>>
using uptr = s::unique_ptr<T, D>;

template<typename T>
using sptr = s::shared_ptr<T>;

template<typename Sig>
using func = s::function<Sig>;

}  // namespace jfsh

#endif  // JFSH_CORE_ALIASES_H
