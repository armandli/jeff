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

const vec<str>& Param::array_ref() const noexcept {
  static const vec<str> NONE;
  if (auto* a = s::get_if<vec<str>>(&value)) return *a;
  return NONE;
}

const umap<str, str>& Param::hash_ref() const noexcept {
  static const umap<str, str> NONE;
  if (auto* h = s::get_if<umap<str, str>>(&value)) return *h;
  return NONE;
}

vec<str> Param::take_array() noexcept {
  if (auto* a = s::get_if<vec<str>>(&value)) return s::move(*a);
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
  // A getter that itself reads the parameter table (zsh/parameter's live
  // $parameters, $functions, …) can recurse through for_each → refresh →
  // get → for_each.  Guard with a per-call-depth thread-local; depth > 1
  // skips the refresh and lets the reader see the cached value.
  static thread_local int depth = 0;
  if (depth > 0) return;
  ++depth;
  if (p.handlers->get_hash) {
    p.value = p.handlers->get_hash();
  } else if (p.handlers->get_array) {
    p.value = p.handlers->get_array();
  } else if (p.handlers->get_scalar) {
    str v = p.handlers->get_scalar();
    if (p.is_integer()) p.value = static_cast<s::int64_t>(s::strtoll(v.c_str(), nullptr, 10));
    else                p.value = s::move(v);
  }
  --depth;
}

// A private parameter (zsh/param/private) is seen only from the scope that
// declared it: a function it calls looks straight through to the outer one.
static bool hidden_private(const Param& p, int scope, int current) noexcept {
  return (p.flags & PM_PRIVATE) and scope != current;
}

optref<Param> ParamTable::lookup_raw(strv name) noexcept {
  for (int i = static_cast<int>(mScopes.size()) - 1; i >= 0; --i) {
    auto it = mScopes[static_cast<s::size_t>(i)].find(str(name));
    if (it != mScopes[static_cast<s::size_t>(i)].end()) {
      if (hidden_private(it->second, i, local_scope())) continue;
      return s::ref(it->second);
    }
  }
  return s::nullopt;
}

optref<Param> ParamTable::lookup(strv name) noexcept {
  auto p = lookup_raw(name);
  if (p) refresh(p->get());
  return p;
}

// Refreshing a special parameter changes only its cached value, so the
// const lookup may do it too.
optref<const Param> ParamTable::lookup(strv name) const noexcept {
  if (auto p = const_cast<ParamTable*>(this)->lookup(name)) return s::cref(p->get());
  return s::nullopt;
}

optref<Param> ParamTable::lookup_local(strv name) noexcept {
  if (mScopes.empty()) return s::nullopt;
  for (int i = current_scope(); i >= local_scope(); --i) {
    auto& scope = mScopes[static_cast<s::size_t>(i)];
    if (auto it = scope.find(str(name)); it != scope.end()) return s::ref(it->second);
  }
  return s::nullopt;
}

bool ParamTable::in_current_scope(const Param& p) const noexcept {
  if (mScopes.empty()) return false;
  auto it = mScopes.back().find(p.name);
  return it != mScopes.back().end() and &it->second == &p;
}

bool ParamTable::in_prefix_scope(const Param& p) const noexcept {
  for (int i : mPrefixScopes) {
    const auto& scope = mScopes[static_cast<s::size_t>(i)];
    if (auto it = scope.find(p.name); it != scope.end() and &it->second == &p) return true;
  }
  return false;
}

int ParamTable::local_scope() const noexcept {
  int i = current_scope();
  for (auto it = mPrefixScopes.rbegin(); it != mPrefixScopes.rend() and *it == i; ++it) --i;
  return i;
}

Param& ParamTable::assign(
    str name, ParamValue value, s::uint32_t flags) {
  note_assigned(name);
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
  note_assigned(name);
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
  note_assigned(name);
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
  note_assigned(name);
  if (mScopes.empty()) mScopes.emplace_back();
  auto& scope = mScopes[static_cast<s::size_t>(local_scope())];
  auto& p = scope[name];
  p.name  = name;
  p.flags = flags;
  p.scope = local_scope();
  return p;
}

Result<void> ParamTable::unset_element(strv name, const str& key) {
  auto existing = lookup_raw(name);
  if (not existing) return {};
  Param& p = existing->get();
  if (p.is_readonly() and not (p.handlers and p.handlers->unset_elem))
    return s::unexpected(ShellError{ErrorKind::ExecError,
        "read-only variable: " + str(name)});
  if (p.handlers and p.handlers->unset_elem) {
    p.handlers->unset_elem(key);
    return {};
  }
  if (not p.is_hash())
    return s::unexpected(ShellError{ErrorKind::ExecError,
        str(name) + ": invalid element for unset"});
  if (auto* h = s::get_if<umap<str, str>>(&p.value)) h->erase(key);
  return {};
}

Result<void> ParamTable::unset(strv name) {
  note_assigned(name);
  for (int i = static_cast<int>(mScopes.size()) - 1; i >= 0; --i) {
    auto& scope = mScopes[static_cast<s::size_t>(i)];
    auto it = scope.find(str(name));
    if (it == scope.end()) continue;
    if (hidden_private(it->second, i, local_scope())) continue;
    if (it->second.is_readonly())
      return s::unexpected(ShellError{ErrorKind::ExecError,
          "read-only variable: " + str(name)});
    if (i == 0) {
      scope.erase(it);
    } else {
      // A local stays local, so the outer parameter doesn't show through.
      it->second.value = str{};
      it->second.flags = PM_SCALAR | PM_UNSET | (it->second.flags & (PM_LOCAL | PM_PRIVATE));
    }
    return {};
  }
  return {};
}

// mScopes[0] is the global scope and is never popped.
void ParamTable::push_scope(bool prefix) {
  if (mScopes.empty()) mScopes.emplace_back();
  mScopes.emplace_back();
  if (prefix) mPrefixScopes.push_back(current_scope());
}

void ParamTable::pop_scope() {
  if (mScopes.size() <= 1) return;
  if (mScopes.back().contains("PATH")) note_assigned("PATH");
  if (not mPrefixScopes.empty() and mPrefixScopes.back() == current_scope())
    mPrefixScopes.pop_back();
  mScopes.pop_back();
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
      if (hidden_private(p, i, local_scope())) continue;
      if (not seen.count(name)) {
        seen[name] = &p;
        refresh(const_cast<Param&>(p));
        fn(p);
      }
    }
  }
}

}  // namespace jfsh
