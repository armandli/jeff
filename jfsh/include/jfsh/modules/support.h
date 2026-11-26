#ifndef JFSH_MODULES_SUPPORT_H
#define JFSH_MODULES_SUPPORT_H
// jfsh/modules/support.h — small helpers shared by the M11 modules.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <string>
#include <string_view>

#include <jfsh/core/aliases.h>
#include <jfsh/core/param.h>
#include <jfsh/exec/executor.h>

namespace jfsh {

// `jfsh: cmd: msg` on stderr; returns 1 for `return mod_error(…)`.
inline int mod_error(strv cmd, strv msg) {
  s::fprintf(stderr, "jfsh: %.*s: %.*s\n",
             static_cast<int>(cmd.size()), cmd.data(),
             static_cast<int>(msg.size()), msg.data());
  return 1;
}

// Removes a parameter a module installed (readonly or not), for the
// module's on_unload.
inline void drop_module_param(ParamTable& params, strv name) {
  if (auto p = params.lookup_raw(name)) {
    p->get().flags &= ~PM_READONLY;
    (void)params.unset(name);
  }
}

// A parameter's scalar value, or `def` when it is unset.
inline str param_or(ParamTable& params, strv name, str def = {}) {
  auto p = params.lookup(name);
  return (p and p->get().is_set()) ? p->get().scalar() : def;
}

// zsh's %e in a message: strerror(err), its first letter lower case unless
// it is about I/O (EIO), "interrupt" for EINTR.
inline str errno_text(int err) {
  if (err == EINTR) return "interrupt";
  str msg = s::strerror(err);
  if (err != EIO and not msg.empty())
    msg[0] = static_cast<char>(s::tolower(static_cast<unsigned char>(msg[0])));
  return msg;
}

// zsh's isident: a positional parameter's number, or an identifier (bytes
// past ASCII count as letters), either with a `[subscript]` after it.
inline bool zsh_isident(strv s) {
  if (s.empty()) return false;
  auto digit = [](char c) { return c >= '0' and c <= '9'; };
  s::size_t i = 0;
  if (digit(s[0])) {
    while (i < s.size() and digit(s[i])) ++i;
  } else {
    auto ident = [&](char c) {
      return digit(c) or (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or c == '_'
          or static_cast<unsigned char>(c) >= 0x80;
    };
    while (i < s.size() and ident(s[i])) ++i;
  }
  if (i == s.size()) return i > 0;
  return i > 0 and s[i] == '[' and s.back() == ']' and s.size() > i + 2;
}

// zsh's setiparam: `name` is assigned as for `name=v`, an integer when it
// is new.  A failure is reported (and ends the command line).
inline bool set_iparam(ExecContext& ctx, const str& name, s::int64_t v) {
  bool fresh = not ctx.params.lookup_raw(name);
  if (not ctx.executor->assign_var(name, s::to_string(v))) return false;
  if (fresh)
    if (auto p = ctx.params.lookup_raw(name)) p->get().set(v);
  return true;
}

// An environment variable set (or unset, with null) for the C library
// while it lives, as zsh's local exported $TZ in bin_strftime.
struct ScopedEnv {
  ScopedEnv(const char* name, const char* value) : name(name) {
    if (const char* old = s::getenv(name)) saved = old;
    if (value) ::setenv(name, value, 1);
    else       ::unsetenv(name);
    ::tzset();
  }
  ~ScopedEnv() {
    if (saved) ::setenv(name, saved->c_str(), 1);
    else       ::unsetenv(name);
    ::tzset();
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

  opt<str>    saved;
  const char* name;
};

}  // namespace jfsh

#endif  // JFSH_MODULES_SUPPORT_H
