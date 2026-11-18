#include <jfsh/modules/files.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <filesystem>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

// zsh/files (M10 Phase 2b): shell builtins for the common POSIX file
// utilities.  Each is a thin wrapper around the matching syscall with
// zsh-style error reporting (`jfsh: NAME: strerror(errno): path`).

namespace jfsh {

namespace {

namespace sf = s::filesystem;

int fail(strv cmd, strv path, int err) {
  s::fprintf(stderr, "jfsh: %.*s: %s: %.*s\n",
             static_cast<int>(cmd.size()), cmd.data(),
             s::strerror(err),
             static_cast<int>(path.size()), path.data());
  return 1;
}

// Parse an octal mode string (0644, 755, 0o700) into mode_t.  Returns -1
// on parse error.
static int parse_mode(strv text) {
  str s(text);
  int base = 8;
  s::size_t start = 0;
  if (s.size() > 2 and s[0] == '0' and (s[1] == 'o' or s[1] == 'O')) start = 2;
  else if (not s.empty() and s[0] == '0') start = 1;
  char* end = nullptr;
  long v = s::strtol(s.c_str() + start, &end, base);
  if (end == s.c_str() + start or *end != '\0') return -1;
  return static_cast<int>(v);
}

// ---------------------------------------------------------------------------
// chmod MODE file…
// ---------------------------------------------------------------------------
int builtin_chmod(vec<str>& args, ExecContext&) {
  if (args.size() < 3) {
    s::fprintf(stderr, "jfsh: chmod: usage: chmod MODE file…\n");
    return 1;
  }
  int mode = parse_mode(args[1]);
  if (mode < 0) {
    s::fprintf(stderr, "jfsh: chmod: bad mode: %s\n", args[1].c_str());
    return 1;
  }
  int rc = 0;
  for (s::size_t i = 2; i < args.size(); ++i)
    if (::chmod(args[i].c_str(), static_cast<mode_t>(mode)) < 0)
      rc = fail("chmod", args[i], errno);
  return rc;
}

// ---------------------------------------------------------------------------
// chown OWNER[:GROUP] file…      chgrp GROUP file…
// OWNER may be a name or numeric uid; GROUP likewise.  `-` means leave
// that end alone.
// ---------------------------------------------------------------------------
static bool resolve_uid(strv text, uid_t& out) {
  if (text.empty() or text == "-") return true;   // sentinel: no change
  if (s::isdigit(static_cast<unsigned char>(text[0]))) {
    out = static_cast<uid_t>(s::strtoul(str(text).c_str(), nullptr, 10));
    return true;
  }
  struct passwd* pw = ::getpwnam(str(text).c_str());
  if (not pw) return false;
  out = pw->pw_uid;
  return true;
}

static bool resolve_gid(strv text, gid_t& out) {
  if (text.empty() or text == "-") return true;
  if (s::isdigit(static_cast<unsigned char>(text[0]))) {
    out = static_cast<gid_t>(s::strtoul(str(text).c_str(), nullptr, 10));
    return true;
  }
  struct group* g = ::getgrnam(str(text).c_str());
  if (not g) return false;
  out = g->gr_gid;
  return true;
}

int builtin_chown(vec<str>& args, ExecContext&) {
  if (args.size() < 3) {
    s::fprintf(stderr, "jfsh: chown: usage: chown OWNER[:GROUP] file…\n");
    return 1;
  }
  const str& spec = args[1];
  s::size_t colon = spec.find(':');
  str user(spec.substr(0, colon));
  str group(colon == str::npos ? "" : spec.substr(colon + 1));
  uid_t uid = static_cast<uid_t>(-1);
  gid_t gid = static_cast<gid_t>(-1);
  if (not user.empty() and user != "-"
      and not resolve_uid(user, uid)) {
    s::fprintf(stderr, "jfsh: chown: unknown user: %s\n", user.c_str());
    return 1;
  }
  if (not group.empty() and group != "-"
      and not resolve_gid(group, gid)) {
    s::fprintf(stderr, "jfsh: chown: unknown group: %s\n", group.c_str());
    return 1;
  }
  int rc = 0;
  for (s::size_t i = 2; i < args.size(); ++i)
    if (::chown(args[i].c_str(), uid, gid) < 0)
      rc = fail("chown", args[i], errno);
  return rc;
}

int builtin_chgrp(vec<str>& args, ExecContext&) {
  if (args.size() < 3) {
    s::fprintf(stderr, "jfsh: chgrp: usage: chgrp GROUP file…\n");
    return 1;
  }
  gid_t gid = static_cast<gid_t>(-1);
  if (not resolve_gid(args[1], gid)) {
    s::fprintf(stderr, "jfsh: chgrp: unknown group: %s\n", args[1].c_str());
    return 1;
  }
  int rc = 0;
  for (s::size_t i = 2; i < args.size(); ++i)
    if (::chown(args[i].c_str(), static_cast<uid_t>(-1), gid) < 0)
      rc = fail("chgrp", args[i], errno);
  return rc;
}

// ---------------------------------------------------------------------------
// ln [-fs] src dst
// ---------------------------------------------------------------------------
int builtin_ln(vec<str>& args, ExecContext&) {
  bool symbolic = false;
  bool force    = false;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a.size() < 2 or a[0] != '-' or a == "--") { if (a == "--") ++i; break; }
    for (s::size_t k = 1; k < a.size(); ++k) {
      if      (a[k] == 's') symbolic = true;
      else if (a[k] == 'f') force    = true;
      else {
        s::fprintf(stderr, "jfsh: ln: bad option: -%c\n", a[k]);
        return 1;
      }
    }
  }
  if (i + 1 >= args.size()) {
    s::fprintf(stderr, "jfsh: ln: usage: ln [-fs] src dst\n");
    return 1;
  }
  const str& src = args[i];
  const str& dst = args[i + 1];
  if (force) ::unlink(dst.c_str());
  int rc = symbolic ? ::symlink(src.c_str(), dst.c_str())
                    : ::link(src.c_str(), dst.c_str());
  if (rc < 0) return fail("ln", dst, errno);
  return 0;
}

// ---------------------------------------------------------------------------
// mkdir [-p] [-m MODE] dir…
// ---------------------------------------------------------------------------
int builtin_mkdir(vec<str>& args, ExecContext&) {
  bool parents = false;
  int  mode    = 0755;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-p") parents = true;
    else if (a == "-m") {
      if (++i >= args.size()) {
        s::fprintf(stderr, "jfsh: mkdir: option -m requires an argument\n");
        return 1;
      }
      int m = parse_mode(args[i]);
      if (m < 0) {
        s::fprintf(stderr, "jfsh: mkdir: bad mode: %s\n", args[i].c_str());
        return 1;
      }
      mode = m;
    } else if (a == "--") { ++i; break; }
    else if (a.size() > 1 and a[0] == '-') {
      s::fprintf(stderr, "jfsh: mkdir: bad option: %s\n", a.c_str());
      return 1;
    } else break;
  }
  if (i >= args.size()) {
    s::fprintf(stderr, "jfsh: mkdir: usage: mkdir [-p] [-m MODE] dir…\n");
    return 1;
  }
  int rc = 0;
  for (; i < args.size(); ++i) {
    s::error_code ec;
    if (parents) {
      sf::create_directories(args[i], ec);
      if (ec) rc = fail("mkdir", args[i], ec.value());
      else if (mode != 0755) ::chmod(args[i].c_str(), static_cast<mode_t>(mode));
    } else if (::mkdir(args[i].c_str(), static_cast<mode_t>(mode)) < 0) {
      rc = fail("mkdir", args[i], errno);
    }
  }
  return rc;
}

// ---------------------------------------------------------------------------
// mv src… dst
// ---------------------------------------------------------------------------
int builtin_mv(vec<str>& args, ExecContext&) {
  if (args.size() < 3) {
    s::fprintf(stderr, "jfsh: mv: usage: mv src… dst\n");
    return 1;
  }
  const str& dst = args.back();
  bool dst_is_dir = sf::is_directory(dst);
  int rc = 0;
  for (s::size_t i = 1; i + 1 < args.size(); ++i) {
    s::error_code ec;
    sf::path target = dst_is_dir ? sf::path(dst) / sf::path(args[i]).filename()
                                 : sf::path(dst);
    sf::rename(args[i], target, ec);
    if (ec) rc = fail("mv", args[i], ec.value());
  }
  return rc;
}

// ---------------------------------------------------------------------------
// rm [-rRf] file…
// ---------------------------------------------------------------------------
int builtin_rm(vec<str>& args, ExecContext&) {
  bool recursive = false;
  bool force     = false;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a.size() < 2 or a[0] != '-' or a == "--") { if (a == "--") ++i; break; }
    for (s::size_t k = 1; k < a.size(); ++k) {
      if      (a[k] == 'r' or a[k] == 'R') recursive = true;
      else if (a[k] == 'f') force = true;
      else if (a[k] == 'v') { /* verbose: ignored */ }
      else {
        s::fprintf(stderr, "jfsh: rm: bad option: -%c\n", a[k]);
        return 1;
      }
    }
  }
  if (i >= args.size() and not force) {
    s::fprintf(stderr, "jfsh: rm: missing operand\n");
    return 1;
  }
  int rc = 0;
  for (; i < args.size(); ++i) {
    s::error_code ec;
    if (recursive) sf::remove_all(args[i], ec);
    else           sf::remove(args[i], ec);
    if (ec and not force) rc = fail("rm", args[i], ec.value());
  }
  return rc;
}

// ---------------------------------------------------------------------------
// rmdir dir…
// ---------------------------------------------------------------------------
int builtin_rmdir(vec<str>& args, ExecContext&) {
  if (args.size() < 2) {
    s::fprintf(stderr, "jfsh: rmdir: usage: rmdir dir…\n");
    return 1;
  }
  int rc = 0;
  for (s::size_t i = 1; i < args.size(); ++i)
    if (::rmdir(args[i].c_str()) < 0)
      rc = fail("rmdir", args[i], errno);
  return rc;
}

// ---------------------------------------------------------------------------
// sync
// ---------------------------------------------------------------------------
int builtin_sync(vec<str>&, ExecContext&) {
  ::sync();
  return 0;
}

}  // namespace

void register_files_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/files", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"chmod",  builtin_chmod,  BINF_NONE, 2});
    out.builtins.push_back(BuiltinDef{"chown",  builtin_chown,  BINF_NONE, 2});
    out.builtins.push_back(BuiltinDef{"chgrp",  builtin_chgrp,  BINF_NONE, 2});
    out.builtins.push_back(BuiltinDef{"ln",     builtin_ln,     BINF_NONE, 2});
    out.builtins.push_back(BuiltinDef{"mkdir",  builtin_mkdir,  BINF_NONE, 1});
    out.builtins.push_back(BuiltinDef{"mv",     builtin_mv,     BINF_NONE, 2});
    out.builtins.push_back(BuiltinDef{"rm",     builtin_rm,     BINF_NONE, 0});
    out.builtins.push_back(BuiltinDef{"rmdir",  builtin_rmdir,  BINF_NONE, 1});
    out.builtins.push_back(BuiltinDef{"sync",   builtin_sync,   BINF_NONE, 0});
  });
}

}  // namespace jfsh
