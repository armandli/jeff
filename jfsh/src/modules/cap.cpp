#include <jfsh/modules/cap.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

#ifdef JFSH_HAVE_LIBCAP
#include <sys/capability.h>
#endif

// zsh/cap (M11): POSIX.1e capabilities, as zsh's Src/Modules/cap.c.
// Only Linux with libcap has them; elsewhere the builtins say so.

namespace jfsh {

namespace {

#ifdef JFSH_HAVE_LIBCAP

// Prints a capability set as text, freeing both.
int print_caps(cap_t caps, strv cmd, const char* prefix = nullptr) {
  if (not caps) return mod_error(cmd, s::strerror(errno));
  char* text = ::cap_to_text(caps, nullptr);
  ::cap_free(caps);
  if (not text) return mod_error(cmd, s::strerror(errno));
  if (prefix) s::printf("%s %s\n", prefix, text);
  else        s::printf("%s\n", text);
  ::cap_free(text);
  return 0;
}

int builtin_cap(vec<str>& args, ExecContext&) {
  if (args.size() < 2) return print_caps(::cap_get_proc(), "cap");
  cap_t caps = ::cap_from_text(args[1].c_str());
  if (not caps) return mod_error("cap", "invalid capability string");
  int rc = ::cap_set_proc(caps);
  ::cap_free(caps);
  return rc == 0 ? 0 : mod_error("cap", s::strerror(errno));
}

int builtin_getcap(vec<str>& args, ExecContext&) {
  int rc = 0;
  for (s::size_t i = 1; i < args.size(); ++i)
    if (print_caps(::cap_get_file(args[i].c_str()), "getcap", args[i].c_str()) != 0) rc = 1;
  return rc;
}

int builtin_setcap(vec<str>& args, ExecContext&) {
  if (args.size() < 3) return mod_error("setcap", "not enough arguments");
  cap_t caps = ::cap_from_text(args[1].c_str());
  if (not caps) return mod_error("setcap", "invalid capability string");
  int rc = 0;
  for (s::size_t i = 2; i < args.size(); ++i)
    if (::cap_set_file(args[i].c_str(), caps) != 0)
      rc = mod_error("setcap", args[i] + ": " + s::strerror(errno));
  ::cap_free(caps);
  return rc;
}

#else

int unsupported(strv cmd) { return mod_error(cmd, "not supported on this system"); }
int builtin_cap   (vec<str>&, ExecContext&) { return unsupported("cap"); }
int builtin_getcap(vec<str>&, ExecContext&) { return unsupported("getcap"); }
int builtin_setcap(vec<str>&, ExecContext&) { return unsupported("setcap"); }

#endif

}  // namespace

void register_cap_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/cap", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"cap",    builtin_cap,    BINF_NONE, 0});
    out.builtins.push_back(BuiltinDef{"getcap", builtin_getcap, BINF_NONE, 1});
    out.builtins.push_back(BuiltinDef{"setcap", builtin_setcap, BINF_NONE, 2});
    out.feature_names = {"b:cap", "b:getcap", "b:setcap"};
  });
}

}  // namespace jfsh
