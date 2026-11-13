#include <algorithm>
#include <iterator>

#include <jfsh/core/param.h>

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

std::optional<std::reference_wrapper<Param>>
ParamTable::lookup_local(std::string_view name) noexcept {
  if (mScopes.empty()) return std::nullopt;
  auto it = mScopes.back().find(std::string(name));
  if (it == mScopes.back().end()) return std::nullopt;
  return std::ref(it->second);
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

Result<void> ParamTable::set_scalar(
    std::string_view name, std::string value, bool append) {
  if (auto existing = lookup(name)) {
    Param& p = existing->get();
    if (p.is_readonly())
      return std::unexpected(ShellError{ErrorKind::ExecError,
          "read-only variable: " + std::string(name)});
    if (append) value = p.scalar() + value;
    p.set(std::move(value));
    return {};
  }
  if (mScopes.empty()) mScopes.emplace_back();
  auto& p = mScopes.front()[std::string(name)];
  p.name  = name;
  p.flags = PM_SCALAR;
  p.scope = 0;
  p.set(std::move(value));
  return {};
}

Result<void> ParamTable::set_array(
    std::string_view name, std::vector<std::string> words, bool append) {
  if (auto existing = lookup(name)) {
    Param& p = existing->get();
    if (p.is_readonly())
      return std::unexpected(ShellError{ErrorKind::ExecError,
          "read-only variable: " + std::string(name)});
    if (p.is_hash()) {
      if (words.size() % 2 != 0)
        return std::unexpected(ShellError{ErrorKind::ExecError,
            "bad set of key/value pairs for associative array"});
      auto map = append ? p.hash() : std::unordered_map<std::string, std::string>{};
      for (std::size_t i = 0; i < words.size(); i += 2)
        map[words[i]] = std::move(words[i + 1]);
      p.set(std::move(map));
      return {};
    }
    if (append) {
      auto old = p.is_array() ? p.array()
               : (p.is_set() ? std::vector<std::string>{p.scalar()}
                             : std::vector<std::string>{});
      old.insert(old.end(), std::make_move_iterator(words.begin()),
                 std::make_move_iterator(words.end()));
      words = std::move(old);
    }
    p.set(std::move(words));
    return {};
  }
  if (mScopes.empty()) mScopes.emplace_back();
  auto& p = mScopes.front()[std::string(name)];
  p.name  = name;
  p.flags = PM_ARRAY;
  p.scope = 0;
  p.set(std::move(words));
  return {};
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

// mScopes[0] is the global scope and is never popped.
void ParamTable::push_scope() {
  if (mScopes.empty()) mScopes.emplace_back();
  mScopes.emplace_back();
}

void ParamTable::pop_scope() {
  if (mScopes.size() > 1) mScopes.pop_back();
}

// The innermost definition of a name decides whether, and with which value,
// it is exported; outer definitions it shadows are skipped.
std::vector<std::string> ParamTable::make_envp() const {
  std::vector<std::string> env;
  for_each([&](const Param& p) {
    if (not p.is_exported() or not p.is_set()) return;
    if (p.is_scalar())       env.push_back(p.name + "=" + p.scalar());
    else if (p.is_integer()) env.push_back(p.name + "=" + std::to_string(p.integer()));
  });
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
