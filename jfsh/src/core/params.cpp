#include <algorithm>

#include <jfsh/core/param.hh>

namespace jfsh {

std::string Param::scalar() const noexcept {
  if (auto* s = std::get_if<std::string>(&value)) return *s;
  return {};
}

long long Param::integer() const noexcept {
  if (auto* i = std::get_if<long long>(&value)) return *i;
  return 0;
}

double Param::floatv() const noexcept {
  if (auto* f = std::get_if<double>(&value)) return *f;
  return 0.0;
}

std::vector<std::string> Param::array() const noexcept {
  if (auto* a = std::get_if<std::vector<std::string>>(&value)) return *a;
  return {};
}

std::unordered_map<std::string, std::string> Param::hash() const noexcept {
  using Map = std::unordered_map<std::string, std::string>;
  if (auto* h = std::get_if<Map>(&value)) return *h;
  return {};
}

void Param::set(std::string s) noexcept {
  value = std::move(s);
  flags = (flags & ~(PM_UNSET | PM_INTEGER | PM_FLOAT | PM_ARRAY | PM_HASH))
        | PM_SCALAR;
}

void Param::set(long long i) noexcept {
  value = i;
  flags = (flags & ~(PM_UNSET | PM_SCALAR | PM_FLOAT | PM_ARRAY | PM_HASH))
        | PM_INTEGER;
}

void Param::set(double f) noexcept {
  value = f;
  flags = (flags & ~(PM_UNSET | PM_SCALAR | PM_INTEGER | PM_ARRAY | PM_HASH))
        | PM_FLOAT;
}

void Param::set(std::vector<std::string> a) noexcept {
  value = std::move(a);
  flags = (flags & ~(PM_UNSET | PM_SCALAR | PM_INTEGER | PM_FLOAT | PM_HASH))
        | PM_ARRAY;
}

void Param::set(std::unordered_map<std::string, std::string> h) noexcept {
  value = std::move(h);
  flags = (flags & ~(PM_UNSET | PM_SCALAR | PM_INTEGER | PM_FLOAT | PM_ARRAY))
        | PM_HASH;
}

std::optional<std::reference_wrapper<Param>>
ParamTable::lookup(std::string_view name) noexcept {
  for (int i = static_cast<int>(mScopes.size()) - 1; i >= 0; --i) {
    auto it = mScopes[static_cast<std::size_t>(i)].find(std::string(name));
    if (it != mScopes[static_cast<std::size_t>(i)].end())
      return std::ref(it->second);
  }
  return std::nullopt;
}

std::optional<std::reference_wrapper<const Param>>
ParamTable::lookup(std::string_view name) const noexcept {
  for (int i = static_cast<int>(mScopes.size()) - 1; i >= 0; --i) {
    auto it = mScopes[static_cast<std::size_t>(i)].find(std::string(name));
    if (it != mScopes[static_cast<std::size_t>(i)].end())
      return std::cref(it->second);
  }
  return std::nullopt;
}

Param& ParamTable::assign(
    std::string name, ParamValue value, std::uint32_t flags) {
  if (mScopes.empty()) mScopes.emplace_back();
  auto& scope = mScopes.back();
  auto& p = scope[name];
  p.name  = name;
  p.value = std::move(value);
  p.flags = flags ? flags : PM_SCALAR;
  p.scope = current_scope();
  return p;
}

Param& ParamTable::declare_local(std::string name, std::uint32_t flags) {
  if (mScopes.empty()) mScopes.emplace_back();
  auto& scope = mScopes.back();
  auto& p = scope[name];
  p.name  = name;
  p.flags = flags;
  p.scope = current_scope();
  return p;
}

void ParamTable::push_scope() { mScopes.emplace_back(); }

void ParamTable::pop_scope() {
  if (not mScopes.empty()) mScopes.pop_back();
}

std::vector<std::string> ParamTable::make_envp() const {
  std::vector<std::string> env;
  for (auto const& scope : mScopes) {
    for (auto const& [name, p] : scope) {
      if (p.is_exported() and p.is_set() and p.is_scalar())
        env.push_back(name + "=" + p.scalar());
    }
  }
  return env;
}

void ParamTable::for_each(std::function<void(const Param&)> fn) const {
  HashTable<const Param*> seen;
  for (int i = static_cast<int>(mScopes.size()) - 1; i >= 0; --i) {
    for (auto const& [name, p] : mScopes[static_cast<std::size_t>(i)]) {
      if (not seen.count(name)) {
        seen[name] = &p;
        fn(p);
      }
    }
  }
}

}  // namespace jfsh
