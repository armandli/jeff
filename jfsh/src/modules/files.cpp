// zsh/files: a port of zsh 5.9's Src/Modules/files.c.  zsh's copyright and
// licence:
//
// Copyright (c) 1996-1997 Andrew Main
// All rights reserved.
//
// Permission is hereby granted, without written agreement and without
// license or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall Andrew Main or the Zsh Development Group be liable
// to any party for direct, indirect, special, incidental, or consequential
// damages arising out of the use of this software and its documentation,
// even if Andrew Main and the Zsh Development Group have been advised of
// the possibility of such damage.
//
// Andrew Main and the Zsh Development Group specifically disclaim any
// warranties, including, but not limited to, the implied warranties of
// merchantability and fitness for a particular purpose.  The software
// provided hereunder is on an "as is" basis, and Andrew Main and the
// Zsh Development Group have no obligation to provide maintenance,
// support, updates, enhancements, or modifications.

#include <jfsh/modules/files.h>

#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

namespace jfsh {

namespace {

void warn_errno(strv nam, const str& what, int err) {
  mod_error(nam, what + ": " + errno_text(err));
}

// A y or n answer read from standard input (zsh's ask).
bool ask() {
  int a = s::getchar();
  for (int c = a; c != EOF and c != '\n';) c = s::getchar();
  return a == 'y' or a == 'Y';
}

unsigned mode_to_octal(mode_t mode) { return static_cast<unsigned>(mode & 07777); }

// What nicezputs writes.
str nice(strv s) {
  str out;
  for (char ch : s) {
    auto c = static_cast<unsigned char>(ch);
    if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else if (c < 0x20 or c == 0x7f) { out += '^'; out += static_cast<char>(c ^ 0x40); }
    else out += ch;
  }
  return out;
}

int bin_sync(vec<str>& args, ExecContext&) {
  ZOps ops;
  if (zsh_options(args, nullptr, 0, 0, ops) < 0) return 1;
  ::sync();
  return 0;
}

int domkdir(strv nam, const str& path, mode_t mode, bool p) {
  int err = 0;
  for (int n = 8; n-- > 0;) {
    mode_t oumask = ::umask(0);
    err = ::mkdir(path.c_str(), mode) ? errno : 0;
    ::umask(oumask);
    if (not err) return 0;
    if (not p or err != EEXIST) break;
    struct stat st{};
    if (::stat(path.c_str(), &st)) {
      if (errno == ENOENT) continue;
      err = errno;
      break;
    }
    if (S_ISDIR(st.st_mode)) return 0;
    break;
  }
  return mod_error(nam, "cannot make directory `" + path + "': " + errno_text(err));
}

// mkdir [-p] [-m mode] dir …
int bin_mkdir(vec<str>& args, ExecContext&) {
  ZOps ops;
  int first = zsh_options(args, "pm:", 1, -1, ops);
  if (first < 0) return 1;
  strv nam = args[0];
  mode_t oumask = ::umask(0);
  mode_t mode = 0777 & ~oumask;
  ::umask(oumask);
  int err = 0;
  if (ops.isset('m')) {
    const str& m = ops.arg('m');
    auto r = zstrtol(m, 0, 8);
    mode = static_cast<mode_t>(r.value);
    if (m.empty() or r.end != m.size()) return mod_error(nam, "invalid mode `" + m + "'");
  }
  for (auto it = args.begin() + first; it != args.end(); ++it) {
    str path = *it;
    while (path.size() > (path[0] == '/' ? 1u : 0u) and path.back() == '/') path.pop_back();
    if (ops.isset('p')) {
      s::size_t ptr = 0;
      for (;;) {
        while (ptr < path.size() and path[ptr] == '/') ++ptr;
        while (ptr < path.size() and path[ptr] != '/') ++ptr;
        if (ptr >= path.size()) {
          err |= domkdir(nam, path, mode, true);
          break;
        }
        if (domkdir(nam, path.substr(0, ptr), mode | 0300, true)) {
          err = 1;
          break;
        }
      }
    } else {
      err |= domkdir(nam, path, mode, false);
    }
  }
  return err;
}

int bin_rmdir(vec<str>& args, ExecContext&) {
  ZOps ops;
  int first = zsh_options(args, nullptr, 1, -1, ops);
  if (first < 0) return 1;
  int err = 0;
  for (auto it = args.begin() + first; it != args.end(); ++it) {
    if (::rmdir(it->c_str())) {
      mod_error(args[0], "cannot remove directory `" + *it + "': " + errno_text(errno));
      err = 1;
    }
  }
  return err;
}

enum MoveFlags : int {
  MV_NODIRS        = 1 << 0,
  MV_FORCE         = 1 << 1,
  MV_INTERACTIVE   = 1 << 2,
  MV_ASKNW         = 1 << 3,
  MV_ATOMIC        = 1 << 4,
  MV_NOCHASETARGET = 1 << 5,
};

using MoveFn = int (*)(const char*, const char*);

int domove(strv nam, MoveFn movefn, const str& p, const str& q, int flags) {
  struct stat st{};
  if (flags & MV_NODIRS) {
    errno = EISDIR;
    if (::lstat(p.c_str(), &st) or S_ISDIR(st.st_mode)) {
      warn_errno(nam, p, errno);
      return 1;
    }
  }
  if (not ::lstat(q.c_str(), &st)) {
    bool doit = flags & MV_FORCE;
    if (S_ISDIR(st.st_mode)) return mod_error(nam, q + ": cannot overwrite directory");
    if (flags & MV_INTERACTIVE) {
      s::fprintf(stderr, "%s: replace `%s'? ", nice(nam).c_str(), nice(q).c_str());
      s::fflush(stderr);
      if (not ask()) return 0;
      doit = true;
    } else if ((flags & MV_ASKNW) and not S_ISLNK(st.st_mode) and ::access(q.c_str(), W_OK)) {
      s::fprintf(stderr, "%s: replace `%s', overriding mode %04o? ", nice(nam).c_str(),
                 nice(q).c_str(), mode_to_octal(st.st_mode));
      s::fflush(stderr);
      if (not ask()) return 0;
      doit = true;
    }
    if (doit and not (flags & MV_ATOMIC)) ::unlink(q.c_str());
  }
  if (movefn(p.c_str(), q.c_str())) {
    int ferrno = errno;
    const str* errfile = &p;
    // p is there, so the error is q's.
    if (ferrno == ENOENT and not ::lstat(p.c_str(), &st)) errfile = &q;
    mod_error(nam, "`" + *errfile + "': " + errno_text(ferrno));
    return 1;
  }
  return 0;
}

int do_rename(const char* a, const char* b) { return ::rename(a, b); }
int do_symlink(const char* a, const char* b) { return ::symlink(a, b); }
int do_link(const char* a, const char* b) { return ::link(a, b); }

// ln [-dfhins] file … and mv [-fi] file …
int bin_ln(vec<str>& argv, bool mv) {
  ZOps ops;
  int first = zsh_options(argv, mv ? "fi" : "dfhins", mv ? 2 : 1, -1, ops);
  if (first < 0) return 1;
  strv nam = argv[0];
  vec<str> args(argv.begin() + first, argv.end());
  MoveFn movefn;
  int flags;
  if (mv) {
    movefn = do_rename;
    flags  = (ops.isset('f') ? 0 : MV_ASKNW) | MV_ATOMIC;
  } else {
    flags = ops.isset('f') ? MV_FORCE : 0;
    if (ops.isset('h') or ops.isset('n')) flags |= MV_NOCHASETARGET;
    if (ops.isset('s')) {
      movefn = do_symlink;
    } else {
      movefn = do_link;
      if (not ops.isset('d')) flags |= MV_NODIRS;
    }
  }
  if (ops.isset('i') and not ops.isset('f')) flags |= MV_INTERACTIVE;
  s::size_t last = args.size() - 1;
  bool have_dir = false;
  if (last > 0) {
    struct stat st{};
    const str& rp = args[last];
    if (not ::stat(rp.c_str(), &st) and S_ISDIR(st.st_mode)) {
      have_dir = true;
      if ((flags & MV_NOCHASETARGET) and not ::lstat(rp.c_str(), &st) and S_ISLNK(st.st_mode)) {
        if (last > 1) {
          warn_errno(nam, rp, ENOTDIR);
          return 1;
        }
        if (flags & MV_FORCE) {
          ::unlink(rp.c_str());
          have_dir = false;
        } else {
          warn_errno(nam, rp, EEXIST);
          return 1;
        }
      }
    }
  }
  if (not have_dir) {
    if (last > 1) return mod_error(nam, "last of many arguments must be a directory");
    if (last == 0) {
      auto slash = args[0].rfind('/');
      args.push_back(slash == str::npos ? args[0] : args[0].substr(slash + 1));
    }
    return domove(nam, movefn, args[0], args[1], flags);
  }
  str dir = args[last] + "/";
  int err = 0;
  for (s::size_t i = 0; i < last; ++i) {
    auto slash = args[i].rfind('/');
    str base = slash == str::npos ? args[i] : args[i].substr(slash + 1);
    err |= domove(nam, movefn, args[i], dir + base, flags);
  }
  return err;
}

// zsh's recursivecmd: each argument, with -R a directory's contents
// first (dirpre), then its entries, then the directory again (dirpost).
// Returns 1 for an error, with 2 set when the walk had to stop.
struct Recursive {
  strv nam;
  func<int(const str& arg, const struct stat* sp)> dirpre, dirpost, leaf;
  bool noerr = false, recurse = false;

  int doone(const str& arg) {
    struct stat st{};
    const struct stat* sp = nullptr;
    if (recurse and not ::lstat(arg.c_str(), &st)) {
      if (S_ISDIR(st.st_mode)) return dorec(arg, st);
      sp = &st;
    }
    return leaf(arg, sp);
  }

  int dorec(const str& arg, const struct stat& st) {
    int err1 = dirpre(arg, &st);
    if (err1 & 2) return 2;
    DIR* d = ::opendir(arg.c_str());
    if (not d) {
      if (not noerr) mod_error(nam, arg + ": " + errno_text(errno));
      return 1;
    }
    int err = err1;
    vec<str> files;
    while (struct dirent* de = ::readdir(d))
      if (s::strcmp(de->d_name, ".") and s::strcmp(de->d_name, ".."))
        files.emplace_back(de->d_name);
    ::closedir(d);
    for (const auto& fn : files) {
      if (err & 2) break;
      err |= doone(arg + "/" + fn);
    }
    if (err & 2) return 2;
    return err | dirpost(arg, &st);
  }

  int run(const vec<str>& args) {
    int err = 0;
    for (const auto& a : args) {
      if (err & 2) break;
      err |= doone(a);
    }
    return err ? 1 : 0;
  }
};

int nothing(const str&, const struct stat*) { return 0; }

// rm [-dfiRrs] file …
int bin_rm(vec<str>& argv, ExecContext&) {
  ZOps ops;
  int first = zsh_options(argv, "dfiRrs", 1, -1, ops);
  if (first < 0) return 1;
  strv nam = argv[0];
  bool force = ops.isset('f'), interact = ops.isset('i') and not force, unlinkdir = ops.isset('d');
  Recursive r;
  r.nam     = nam;
  r.noerr   = force;
  r.recurse = not unlinkdir and (ops.isset('R') or ops.isset('r'));
  r.dirpre  = nothing;
  r.leaf    = [&](const str& arg, const struct stat* sp) {
    struct stat st{};
    if (not unlinkdir or not force) {
      if (not sp and not ::lstat(arg.c_str(), &st)) sp = &st;
      if (sp) {
        if (not unlinkdir and S_ISDIR(sp->st_mode)) {
          if (force) return 0;
          warn_errno(nam, arg, EISDIR);
          return 1;
        }
        if (interact) {
          s::fprintf(stderr, "%s: remove `%s'? ", nice(nam).c_str(), nice(arg).c_str());
          s::fflush(stderr);
          if (not ask()) return 0;
        } else if (not force and not S_ISLNK(sp->st_mode) and ::access(arg.c_str(), W_OK)) {
          s::fprintf(stderr, "%s: remove `%s', overriding mode %04o? ", nice(nam).c_str(),
                     nice(arg).c_str(), mode_to_octal(sp->st_mode));
          s::fflush(stderr);
          if (not ask()) return 0;
        }
      }
    }
    if (::unlink(arg.c_str()) and not force) {
      warn_errno(nam, arg, errno);
      return 1;
    }
    return 0;
  };
  r.dirpost = [&](const str& arg, const struct stat*) {
    if (interact) {
      s::fprintf(stderr, "%s: remove `%s'? ", nice(nam).c_str(), nice(arg).c_str());
      s::fflush(stderr);
      if (not ask()) return 0;
    }
    if (::rmdir(arg.c_str()) and not force) {
      warn_errno(nam, arg, errno);
      return 1;
    }
    return 0;
  };
  int err = r.run(vec<str>(argv.begin() + first, argv.end()));
  return force ? 0 : err;
}

// chmod [-Rs] mode file …: an octal mode only.
int bin_chmod(vec<str>& argv, ExecContext&) {
  ZOps ops;
  int first = zsh_options(argv, "Rs", 2, -1, ops);
  if (first < 0) return 1;
  strv nam = argv[0];
  const str& m = argv[static_cast<s::size_t>(first)];
  auto parsed = zstrtol(m, 0, 8);
  if (m.empty() or parsed.end != m.size()) return mod_error(nam, "invalid mode `" + m + "'");
  auto mode = static_cast<mode_t>(parsed.value);
  Recursive r;
  r.nam     = nam;
  r.recurse = ops.isset('R');
  auto dochmod = [&](const str& arg, const struct stat*) {
    if (::chmod(arg.c_str(), mode)) {
      warn_errno(nam, arg, errno);
      return 1;
    }
    return 0;
  };
  r.dirpre  = dochmod;
  r.dirpost = nothing;
  r.leaf    = dochmod;
  return r.run(vec<str>(argv.begin() + first + 1, argv.end()));
}

// A number, all of the text (zsh's getnumeric).
opt<unsigned long> getnumeric(const str& p) {
  if (p.empty() or not s::isdigit(static_cast<unsigned char>(p[0]))) return s::nullopt;
  char* end = nullptr;
  unsigned long ret = s::strtoul(p.c_str(), &end, 10);
  if (*end) return s::nullopt;
  return ret;
}

// chown [-hRs] user[:group] file … and chgrp [-hRs] group file …
int bin_chown(vec<str>& argv, bool chgrp) {
  ZOps ops;
  int first = zsh_options(argv, "hRs", 2, -1, ops);
  if (first < 0) return 1;
  strv nam = argv[0];
  str uspec = argv[static_cast<s::size_t>(first)];
  uid_t uid = static_cast<uid_t>(-1);
  gid_t gid = static_cast<gid_t>(-1);
  auto group = [&](const str& p) {
    if (struct group* grp = ::getgrnam(p.c_str())) {
      gid = grp->gr_gid;
      return true;
    }
    auto n = getnumeric(p);
    if (not n) {
      mod_error(nam, p + ": no such group");
      return false;
    }
    gid = static_cast<gid_t>(*n);
    return true;
  };
  if (chgrp) {
    if (not group(uspec)) return 1;
  } else {
    auto end = uspec.find(':');
    if (end == str::npos) end = uspec.find('.');
    if (end == 0) {
      if (not group(uspec.substr(1))) return 1;
    } else {
      str user = uspec.substr(0, end);
      struct passwd* pwd = ::getpwnam(user.c_str());
      if (pwd) {
        uid = pwd->pw_uid;
      } else if (auto n = getnumeric(user)) {
        uid = static_cast<uid_t>(*n);
      } else {
        return mod_error(nam, user + ": no such user");
      }
      if (end != str::npos) {
        str p = uspec.substr(end + 1);
        if (p.empty()) {
          if (not pwd and not (pwd = ::getpwuid(uid)))
            return mod_error(nam, user + ": no such user");
          gid = pwd->pw_gid;
        } else if (p != ":") {
          if (not group(p)) return 1;
        }
      }
    }
  }
  bool h = ops.isset('h');
  Recursive r;
  r.nam     = nam;
  r.recurse = ops.isset('R');
  auto dochown = [&](const str& arg, const struct stat*) {
    if ((h ? ::lchown(arg.c_str(), uid, gid) : ::chown(arg.c_str(), uid, gid))) {
      warn_errno(nam, arg, errno);
      return 1;
    }
    return 0;
  };
  r.dirpre  = dochown;
  r.dirpost = nothing;
  r.leaf    = dochown;
  return r.run(vec<str>(argv.begin() + first + 1, argv.end()));
}

}  // namespace

void register_files_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/files", [](ModuleFeatures& out, ExecContext&) {
    auto chgrp = [](vec<str>& a, ExecContext&) { return bin_chown(a, true); };
    auto chown = [](vec<str>& a, ExecContext&) { return bin_chown(a, false); };
    auto ln    = [](vec<str>& a, ExecContext&) { return bin_ln(a, false); };
    auto mv    = [](vec<str>& a, ExecContext&) { return bin_ln(a, true); };
    // The names that overlap commands, then the "safe" zsh-only ones.
    out.builtins = {
      {"chgrp", chgrp, BINF_NONE},    {"chmod", bin_chmod, BINF_NONE},
      {"chown", chown, BINF_NONE},    {"ln", ln, BINF_NONE},
      {"mkdir", bin_mkdir, BINF_NONE}, {"mv", mv, BINF_NONE},
      {"rm", bin_rm, BINF_NONE},      {"rmdir", bin_rmdir, BINF_NONE},
      {"sync", bin_sync, BINF_NONE},
      {"zf_chgrp", chgrp, BINF_NONE},    {"zf_chmod", bin_chmod, BINF_NONE},
      {"zf_chown", chown, BINF_NONE},    {"zf_ln", ln, BINF_NONE},
      {"zf_mkdir", bin_mkdir, BINF_NONE}, {"zf_mv", mv, BINF_NONE},
      {"zf_rm", bin_rm, BINF_NONE},      {"zf_rmdir", bin_rmdir, BINF_NONE},
      {"zf_sync", bin_sync, BINF_NONE},
    };
  });
}

}  // namespace jfsh
