#include <jfsh/modules/attr.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/types.h>
#if __has_include(<sys/xattr.h>)
#include <sys/xattr.h>
#define JFSH_HAVE_XATTR 1
#endif

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

// zsh/attr (M11): extended attributes, as zsh's Src/Modules/attr.c.
// macOS's calls take a position and an options word (XATTR_NOFOLLOW for
// -h); Linux has separate l* calls for symlinks.

namespace jfsh {

namespace {

#ifdef JFSH_HAVE_XATTR

ssize_t x_get(const str& f, const str& n, void* buf, s::size_t size, bool nofollow) {
#ifdef __APPLE__
  return ::getxattr(f.c_str(), n.c_str(), buf, size, 0, nofollow ? XATTR_NOFOLLOW : 0);
#else
  return nofollow ? ::lgetxattr(f.c_str(), n.c_str(), buf, size)
                  : ::getxattr(f.c_str(), n.c_str(), buf, size);
#endif
}

int x_set(const str& f, const str& n, const str& v, bool nofollow) {
#ifdef __APPLE__
  return ::setxattr(f.c_str(), n.c_str(), v.data(), v.size(), 0, nofollow ? XATTR_NOFOLLOW : 0);
#else
  return nofollow ? ::lsetxattr(f.c_str(), n.c_str(), v.data(), v.size(), 0)
                  : ::setxattr(f.c_str(), n.c_str(), v.data(), v.size(), 0);
#endif
}

int x_del(const str& f, const str& n, bool nofollow) {
#ifdef __APPLE__
  return ::removexattr(f.c_str(), n.c_str(), nofollow ? XATTR_NOFOLLOW : 0);
#else
  return nofollow ? ::lremovexattr(f.c_str(), n.c_str()) : ::removexattr(f.c_str(), n.c_str());
#endif
}

ssize_t x_list(const str& f, char* buf, s::size_t size, bool nofollow) {
#ifdef __APPLE__
  return ::listxattr(f.c_str(), buf, size, nofollow ? XATTR_NOFOLLOW : 0);
#else
  return nofollow ? ::llistxattr(f.c_str(), buf, size) : ::listxattr(f.c_str(), buf, size);
#endif
}

// Strips a leading -h; returns whether it was there.
bool take_h(vec<str>& args, s::size_t& i) {
  if (i < args.size() and args[i] == "-h") { ++i; return true; }
  return false;
}

int fail(strv cmd, const str& file) { return mod_error(cmd, file + ": " + s::strerror(errno)); }

int builtin_zgetattr(vec<str>& args, ExecContext& ctx) {
  s::size_t i = 1;
  bool h = take_h(args, i);
  if (args.size() - i < 2 or args.size() - i > 3) return mod_error("zgetattr", "wrong number of arguments");
  const str& file = args[i];
  const str& name = args[i + 1];
  ssize_t len = x_get(file, name, nullptr, 0, h);
  if (len < 0) return fail("zgetattr", file);
  str value(static_cast<s::size_t>(len), '\0');
  len = x_get(file, name, value.data(), value.size(), h);
  if (len < 0) return fail("zgetattr", file);
  value.resize(static_cast<s::size_t>(len));
  if (args.size() - i == 3) (void)ctx.params.set_scalar(args[i + 2], value);
  else s::printf("%s\n", value.c_str());
  return 0;
}

int builtin_zsetattr(vec<str>& args, ExecContext&) {
  s::size_t i = 1;
  bool h = take_h(args, i);
  if (args.size() - i != 3) return mod_error("zsetattr", "wrong number of arguments");
  if (x_set(args[i], args[i + 1], args[i + 2], h) != 0) return fail("zsetattr", args[i]);
  return 0;
}

int builtin_zdelattr(vec<str>& args, ExecContext&) {
  s::size_t i = 1;
  bool h = take_h(args, i);
  if (args.size() - i < 2) return mod_error("zdelattr", "wrong number of arguments");
  int rc = 0;
  for (s::size_t k = i + 1; k < args.size(); ++k)
    if (x_del(args[i], args[k], h) != 0) rc = fail("zdelattr", args[i]);
  return rc;
}

int builtin_zlistattr(vec<str>& args, ExecContext& ctx) {
  s::size_t i = 1;
  bool h = take_h(args, i);
  if (args.size() - i < 1 or args.size() - i > 2) return mod_error("zlistattr", "wrong number of arguments");
  const str& file = args[i];
  ssize_t len = x_list(file, nullptr, 0, h);
  if (len < 0) return fail("zlistattr", file);
  str buf(static_cast<s::size_t>(len), '\0');
  len = x_list(file, buf.data(), buf.size(), h);
  if (len < 0) return fail("zlistattr", file);
  vec<str> names;
  for (s::size_t k = 0; k < static_cast<s::size_t>(len);) {
    str n = buf.c_str() + k;
    k += n.size() + 1;
    if (not n.empty()) names.push_back(s::move(n));
  }
  if (args.size() - i == 2) {
    if (names.size() == 1) (void)ctx.params.set_scalar(args[i + 1], names[0]);
    else (void)ctx.params.set_array(args[i + 1], names);
  } else {
    for (const auto& n : names) s::printf("%s\n", n.c_str());
  }
  return 0;
}

#else

int none(strv cmd) { return mod_error(cmd, "extended attributes are not supported on this system"); }
int builtin_zgetattr (vec<str>&, ExecContext&) { return none("zgetattr"); }
int builtin_zsetattr (vec<str>&, ExecContext&) { return none("zsetattr"); }
int builtin_zdelattr (vec<str>&, ExecContext&) { return none("zdelattr"); }
int builtin_zlistattr(vec<str>&, ExecContext&) { return none("zlistattr"); }

#endif

}  // namespace

void register_attr_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/attr", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"zgetattr",  builtin_zgetattr,  BINF_NONE, 2});
    out.builtins.push_back(BuiltinDef{"zsetattr",  builtin_zsetattr,  BINF_NONE, 3});
    out.builtins.push_back(BuiltinDef{"zdelattr",  builtin_zdelattr,  BINF_NONE, 2});
    out.builtins.push_back(BuiltinDef{"zlistattr", builtin_zlistattr, BINF_NONE, 1});
  });
}

}  // namespace jfsh
