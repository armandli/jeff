#include <cctype>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <iterator>

#include <jfsh/core/aliases.h>
#include <jfsh/core/param.h>

namespace jfsh {

str Param::scalar() const noexcept {
  if (auto* s = s::get_if<str>(&value)) return *s;
  if (auto* i = s::get_if<s::int64_t>(&value)) return s::to_string(*i);
  if (auto* f = s::get_if<double>(&value)) {
    char buf[64];
    s::snprintf(buf, sizeof buf, (flags & PM_EFLOAT) ? "%.9e" : "%.10f", *f);
    return buf;
  }
  return {};
}

s::int64_t Param::integer() const noexcept {
  if (auto* i = s::get_if<s::int64_t>(&value)) return *i;
  return 0;
}

double Param::floatv() const noexcept {
  if (auto* f = s::get_if<double>(&value)) return *f;
  return 0.0;
}

vec<str> Param::array() const noexcept {
  if (auto* a = s::get_if<vec<str>>(&value)) return *a;
  return {};
}

umap<str, str> Param::hash() const noexcept {
  using Map = umap<str, str>;
  if (auto* h = s::get_if<Map>(&value)) return *h;
  return {};
}

// A special parameter's value is computed when it is read ($SECONDS,
// $RANDOM, …): its getter refreshes the stored value, so every reader
// (expansion, arithmetic, typeset -p, the environment) sees the same thing.
static void refresh(Param& p) noexcept {
  if (not p.handlers or not p.is_set()) return;
  if (p.handlers->get_array) {
    p.value = p.handlers->get_array();
  } else if (p.handlers->get_scalar) {
    str v = p.handlers->get_scalar();
    if (p.is_integer()) p.value = static_cast<s::int64_t>(s::strtoll(v.c_str(), nullptr, 10));
    else                p.value = s::move(v);
  }
}

optref<Param> ParamTable::lookup(strv name) noexcept {
  for (int i = static_cast<int>(mScopes.size()) - 1; i >= 0; --i) {
    auto it = mScopes[static_cast<s::size_t>(i)].find(str(name));
    if (it != mScopes[static_cast<s::size_t>(i)].end()) {
      refresh(it->second);
      return s::ref(it->second);
    }
  }
  return s::nullopt;
}

// Refreshing a special parameter changes only its cached value, so the
// const lookup may do it too.
optref<const Param> ParamTable::lookup(strv name) const noexcept {
  if (auto p = const_cast<ParamTable*>(this)->lookup(name)) return s::cref(p->get());
  return s::nullopt;
}

optref<Param> ParamTable::lookup_local(strv name) noexcept {
  if (mScopes.empty()) return s::nullopt;
  auto it = mScopes.back().find(str(name));
  if (it == mScopes.back().end()) return s::nullopt;
  return s::ref(it->second);
}

Param& ParamTable::assign(
    str name, ParamValue value, s::uint32_t flags) {
  if (mScopes.empty()) mScopes.emplace_back();
  auto& scope = mScopes.back();
  auto& p = scope[name];
  p.name  = name;
  p.value = s::move(value);
  p.flags = flags ? flags : PM_SCALAR;
  p.scope = current_scope();
  return p;
}

Result<void> ParamTable::set_scalar(
    strv name, str value, bool append) {
  if (auto existing = lookup(name)) {
    Param& p = existing->get();
    if (p.is_readonly())
      return s::unexpected(ShellError{ErrorKind::ExecError,
          "read-only variable: " + str(name)});
    if (append) value = p.scalar() + value;
    if (p.handlers and p.handlers->set_scalar) {
      p.handlers->set_scalar(s::move(value));   // read back when next looked up
      return {};
    }
    // typeset -u / -l stay in force.
    if (p.flags & PM_UPPER)
      for (char& ch : value) ch = static_cast<char>(s::toupper(static_cast<unsigned char>(ch)));
    if (p.flags & PM_LOWER)
      for (char& ch : value) ch = static_cast<char>(s::tolower(static_cast<unsigned char>(ch)));
    p.set(s::move(value));
    return {};
  }
  if (mScopes.empty()) mScopes.emplace_back();
  auto& p = mScopes.front()[str(name)];
  p.name  = name;
  p.flags = PM_SCALAR;
  p.scope = 0;
  p.set(s::move(value));
  return {};
}

Result<void> ParamTable::set_array(
    strv name, vec<str> words, bool append) {
  if (auto existing = lookup(name)) {
    Param& p = existing->get();
    if (p.is_readonly())
      return s::unexpected(ShellError{ErrorKind::ExecError,
          "read-only variable: " + str(name)});
    if (p.handlers and p.handlers->set_array) {
      if (append) {
        auto old = p.array();
        old.insert(old.end(), words.begin(), words.end());
        words = s::move(old);
      }
      p.handlers->set_array(s::move(words));
      return {};
    }
    if (p.is_hash()) {
      if (words.size() % 2 != 0)
        return s::unexpected(ShellError{ErrorKind::ExecError,
            "bad set of key/value pairs for associative array"});
      auto map = append ? p.hash() : umap<str, str>{};
      for (s::size_t i = 0; i < words.size(); i += 2)
        map[words[i]] = s::move(words[i + 1]);
      p.set(s::move(map));
      return {};
    }
    if (append) {
      auto old = p.is_array() ? p.array()
               : (p.is_set() ? vec<str>{p.scalar()}
                             : vec<str>{});
      old.insert(old.end(), s::make_move_iterator(words.begin()),
                 s::make_move_iterator(words.end()));
      words = s::move(old);
    }
    // typeset -U: only the first of each value is kept.
    if (p.flags & PM_UNIQUE) {
      vec<str> kept;
      for (auto& w : words)
        if (s::find(kept.begin(), kept.end(), w) == kept.end()) kept.push_back(s::move(w));
      words = s::move(kept);
    }
    p.set(s::move(words));
    return {};
  }
  if (mScopes.empty()) mScopes.emplace_back();
  auto& p = mScopes.front()[str(name)];
  p.name  = name;
  p.flags = PM_ARRAY;
  p.scope = 0;
  p.set(s::move(words));
  return {};
}

Param& ParamTable::declare_local(str name, s::uint32_t flags) {
  if (mScopes.empty()) mScopes.emplace_back();
  auto& scope = mScopes.back();
  auto& p = scope[name];
  p.name  = name;
  p.flags = flags;
  p.scope = current_scope();
  return p;
}

Result<void> ParamTable::unset(strv name) {
  for (int i = static_cast<int>(mScopes.size()) - 1; i >= 0; --i) {
    auto& scope = mScopes[static_cast<s::size_t>(i)];
    auto it = scope.find(str(name));
    if (it == scope.end()) continue;
    if (it->second.is_readonly())
      return s::unexpected(ShellError{ErrorKind::ExecError,
          "read-only variable: " + str(name)});
    if (i == 0) {
      scope.erase(it);
    } else {
      // A local stays local, so the outer parameter doesn't show through.
      it->second.value = str{};
      it->second.flags = PM_SCALAR | PM_UNSET | (it->second.flags & PM_LOCAL);
    }
    return {};
  }
  return {};
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
vec<str> ParamTable::make_envp() const {
  vec<str> env;
  for_each([&](const Param& p) {
    if (not p.is_exported() or not p.is_set()) return;
    if (p.is_scalar())       env.push_back(p.name + "=" + p.scalar());
    else if (p.is_integer()) env.push_back(p.name + "=" + s::to_string(p.integer()));
    else if (p.is_float())   env.push_back(p.name + "=" + p.scalar());
  });
  return env;
}

void ParamTable::for_each(func<void(const Param&)> fn) const {
  HashTable<const Param*> seen;
  for (int i = static_cast<int>(mScopes.size()) - 1; i >= 0; --i) {
    for (auto const& [name, p] : mScopes[static_cast<s::size_t>(i)]) {
      if (not seen.count(name)) {
        seen[name] = &p;
        refresh(const_cast<Param&>(p));
        fn(p);
      }
    }
  }
}

}  // namespace jfsh
