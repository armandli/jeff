// zsh/stat: a port of zsh 5.9's Src/Modules/stat.c.  zsh's copyright and
// licence:
//
// Copyright (c) 1996-1997 Peter Stephenson
// All rights reserved.
//
// Permission is hereby granted, without written agreement and without
// license or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall Peter Stephenson or the Zsh Development Group be liable
// to any party for direct, indirect, special, incidental, or consequential
// damages arising out of the use of this software and its documentation,
// even if Peter Stephenson and the Zsh Development Group have been advised of
// the possibility of such damage.
//
// Peter Stephenson and the Zsh Development Group specifically disclaim any
// warranties, including, but not limited to, the implied warranties of
// merchantability and fitness for a particular purpose.  The software
// provided hereunder is on an "as is" basis, and Peter Stephenson and the
// Zsh Development Group have no obligation to provide maintenance,
// support, updates, enhancements, or modifications.

#include <jfsh/modules/stat.h>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>
#include <jfsh/prompt/prompt.h>

namespace jfsh {

namespace {

enum StatNum : int {
  ST_DEV, ST_INO, ST_MODE, ST_NLINK, ST_UID, ST_GID, ST_RDEV, ST_SIZE, ST_ATIM, ST_MTIM,
  ST_CTIM, ST_BLKSIZE, ST_BLOCKS, ST_READLINK, ST_COUNT,
};

enum StatFlags : int {
  STF_NAME = 1, STF_FILE = 2, STF_STRING = 4, STF_RAW = 8, STF_PICK = 16, STF_ARRAY = 32,
  STF_GMT = 64, STF_HASH = 128, STF_OCTAL = 256,
};

constexpr const char* STATELTS[] = {
  "device", "inode", "mode", "nlink", "uid", "gid", "rdev", "size", "atime", "mtime", "ctime",
  "blksize", "blocks", "link",
};

str statmodeprint(mode_t mode, int flags) {
  str out;
  if (flags & STF_RAW) {
    char buf[32];
    s::snprintf(buf, sizeof buf, (flags & STF_OCTAL) ? "0%lo" : "%lu",
                static_cast<unsigned long>(mode));
    out = buf;
    if (flags & STF_STRING) out += " (";
  }
  if (flags & STF_STRING) {
    constexpr const char* modes = "?rwxrwxrwx";
    constexpr mode_t mflags[9] = {S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP,
                                  S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};
    char pm[11];
    pm[0] = S_ISBLK(mode) ? 'b' : S_ISCHR(mode) ? 'c' : S_ISDIR(mode) ? 'd' : S_ISFIFO(mode) ? 'p'
          : S_ISLNK(mode) ? 'l' : S_ISREG(mode) ? '-' : S_ISSOCK(mode) ? 's' : '?';
    for (int i = 1; i <= 9; ++i) pm[i] = (mode & mflags[i - 1]) ? modes[i] : '-';
    pm[10] = '\0';
    if (mode & S_ISUID) pm[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID) pm[6] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX) pm[9] = (mode & S_IXOTH) ? 't' : 'T';
    out += pm;
    if (flags & STF_RAW) out += ")";
  }
  return out;
}

str statidprint(unsigned long id, const char* name, int flags) {
  str out;
  if (flags & STF_RAW) {
    out = s::to_string(id);
    if (flags & STF_STRING) out += " (";
  }
  if (flags & STF_STRING) {
    out += name ? str(name) : s::to_string(id);
    if (flags & STF_RAW) out += ")";
  }
  return out;
}

str stattimeprint(time_t tim, long nsecs, int flags, const str& timefmt) {
  str out;
  if (flags & STF_RAW) {
    out = s::to_string(static_cast<long>(tim));
    if (flags & STF_STRING) out += " (";
  }
  if (flags & STF_STRING) {
    struct tm tm{};
    if (flags & STF_GMT) ::gmtime_r(&tim, &tm);
    else                 ::localtime_r(&tim, &tm);
    // zsh formats into 40 bytes.
    str t = ztrftime(timefmt, tm, nsecs);
    out += t.size() < 40 ? t : str{};
    if (flags & STF_RAW) out += ")";
  }
  return out;
}

str statprint(const struct stat& sb, const str* fname, int iwhich, int flags, const str& timefmt) {
  str out;
  if (flags & STF_NAME) {
    char buf[32];
    s::snprintf(buf, sizeof buf, (flags & (STF_PICK | STF_ARRAY)) ? "%s " : "%-8s",
                STATELTS[iwhich]);
    out = buf;
  }
  auto ul = [](auto v) { return s::to_string(static_cast<unsigned long long>(v)); };
  switch (iwhich) {
    case ST_DEV:   out += ul(sb.st_dev);

    break; case ST_INO:   out += ul(sb.st_ino);

    break; case ST_MODE:  out += statmodeprint(sb.st_mode, flags);

    break; case ST_NLINK: out += ul(sb.st_nlink);

    break; case ST_UID: {
      struct passwd* pw = (flags & STF_STRING) ? ::getpwuid(sb.st_uid) : nullptr;
      out += statidprint(sb.st_uid, pw ? pw->pw_name : nullptr, flags);
    }

    break; case ST_GID: {
      struct group* gr = (flags & STF_STRING) ? ::getgrgid(sb.st_gid) : nullptr;
      out += statidprint(sb.st_gid, gr ? gr->gr_name : nullptr, flags);
    }

    break; case ST_RDEV: out += ul(sb.st_rdev);

    break; case ST_SIZE: out += s::to_string(static_cast<long long>(sb.st_size));
#ifdef __APPLE__

    break; case ST_ATIM: out += stattimeprint(sb.st_atime, sb.st_atimespec.tv_nsec, flags, timefmt);

    break; case ST_MTIM: out += stattimeprint(sb.st_mtime, sb.st_mtimespec.tv_nsec, flags, timefmt);

    break; case ST_CTIM: out += stattimeprint(sb.st_ctime, sb.st_ctimespec.tv_nsec, flags, timefmt);
#else

    break; case ST_ATIM: out += stattimeprint(sb.st_atime, sb.st_atim.tv_nsec, flags, timefmt);

    break; case ST_MTIM: out += stattimeprint(sb.st_mtime, sb.st_mtim.tv_nsec, flags, timefmt);

    break; case ST_CTIM: out += stattimeprint(sb.st_ctime, sb.st_ctim.tv_nsec, flags, timefmt);
#endif

    break; case ST_BLKSIZE: out += ul(sb.st_blksize);

    break; case ST_BLOCKS:  out += ul(sb.st_blocks);

    break; case ST_READLINK:
      if (fname and S_ISLNK(sb.st_mode)) {
        char buf[PATH_MAX + 1];
        auto n = ::readlink(fname->c_str(), buf, PATH_MAX);
        if (n > 0) out.append(buf, static_cast<s::size_t>(n));
      }

    break; default:;
  }
  return out;
}

// zstat (and stat) [-gnNolLtTrs] [-f fd] [-H hash] [-A array] [-F fmt]
// [+element] [file …]
int bin_stat(vec<str>& argv, ExecContext& ctx) {
  ZOps none;
  int first = zsh_options(argv, nullptr, 0, -1, none);
  if (first < 0) return 1;
  strv name = argv[0];
  vec<str> args(argv.begin() + first, argv.end());
  bool o[128] = {};
  int iwhich = -1, ret = 0, flags = 0, fd = 0, found = 0;
  opt<str> arrnam, hashnam;
  str timefmt = "%a %b %e %k:%M:%S %Z %Y";
  s::size_t a = 0;
  for (; a < args.size() and not args[a].empty() and (args[a][0] == '+' or args[a][0] == '-');
       ++a) {
    strv arg = strv(args[a]).substr(1);
    if (arg.empty() or arg[0] == '-' or arg[0] == '+') {
      ++a;
      break;
    }
    if (args[a][0] == '+') {
      if (found) break;
      for (int i = 0; i < ST_COUNT; ++i) {
        if (strv(STATELTS[i]).starts_with(arg)) {
          ++found;
          iwhich = i;
        }
      }
      if (found > 1) return mod_error(name, str(arg) + ": ambiguous stat element");
      if (found == 0) return mod_error(name, str(arg) + ": no such stat element");
      if (iwhich == ST_READLINK) o['L'] = true;
      flags |= STF_PICK;
      continue;
    }
    for (s::size_t k = 0; k < arg.size(); ++k) {
      char c = arg[k];
      auto value = [&](const char* missing) -> opt<str> {
        if (k + 1 < arg.size()) return str(arg.substr(k + 1));
        if (a + 1 >= args.size()) {
          mod_error(name, missing);
          return s::nullopt;
        }
        return args[++a];
      };
      if (s::strchr("glLnNorstT", c)) {
        o[static_cast<unsigned char>(c)] = true;
      } else if (c == 'A') {
        if (not (arrnam = value("missing parameter name"))) return 1;
        flags |= STF_ARRAY;
        break;
      } else if (c == 'H') {
        if (not (hashnam = value("missing parameter name"))) return 1;
        flags |= STF_HASH;
        break;
      } else if (c == 'f') {
        o['f'] = true;
        auto sfd = value("missing file descriptor");
        if (not sfd) return 1;
        auto r = zstrtol(*sfd, 0, 10);
        if (r.end != sfd->size()) return mod_error(name, "bad file descriptor");
        fd = static_cast<int>(r.value);
        break;
      } else if (c == 'F') {
        auto fmt = value("missing time format");
        if (not fmt) return 1;
        timefmt = *fmt;
        o['s'] = true;
        break;
      } else {
        return mod_error(name, str("bad option: -") + c);
      }
    }
  }
  args.erase(args.begin(), args.begin() + static_cast<s::ptrdiff_t>(a));
  if ((flags & STF_ARRAY) and (flags & STF_HASH))
    return mod_error(name, "both array and hash requested");
  auto set_array = [&](const str& nm, vec<str> v) {
    if (auto r = ctx.params.set_array(nm, s::move(v)); not r) {
      s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      return false;
    }
    return true;
  };
  if (o['l']) {
    if (arrnam) {
      vec<str> v(s::begin(STATELTS), s::end(STATELTS));
      return set_array(*arrnam, s::move(v)) ? 0 : 1;
    }
    for (int i = 0; i < ST_COUNT; ++i) s::printf("%s%s", STATELTS[i], i + 1 < ST_COUNT ? " " : "");
    s::putchar('\n');
    return 0;
  }
  if (args.empty() and not o['f']) return mod_error(name, "no files given");
  if (not args.empty() and o['f']) return mod_error(name, "no files allowed with -f");
  s::size_t nargs = o['f'] ? 1 : args.size();
  if (o['g']) {
    flags |= STF_GMT;
    o['s'] = true;
  }
  if (o['s'] or o['r']) flags |= STF_STRING;
  if (o['r'] or not o['s']) flags |= STF_RAW;
  if (o['n']) flags |= STF_FILE;
  if (o['o']) flags |= STF_OCTAL;
  if (o['t']) flags |= STF_NAME;
  if (not (arrnam or hashnam)) {
    if (nargs > 1) flags |= STF_FILE;
    if (not (flags & STF_PICK)) flags |= STF_NAME;
  }
  if (o['N'] or o['f']) flags &= ~STF_FILE;
  if (o['T'] or o['H']) flags &= ~STF_NAME;
  if (hashnam and nargs > 1) return mod_error(name, "only one file allowed with -H");
  vec<str> array, hash;
  for (s::size_t i = 0; o['f'] or i < args.size(); ++i) {
    struct stat sb{};
    const str* file = o['f'] ? nullptr : &args[i];
    int rval = o['f']   ? ::fstat(fd, &sb)
             : o['L']   ? ::lstat(file->c_str(), &sb)
                        : ::stat(file->c_str(), &sb);
    if (rval) {
      mod_error(name, (o['f'] ? s::to_string(fd) : *file) + ": " + errno_text(errno));
      ret = 1;
      if (o['f'] or arrnam) break;
      continue;
    }
    if (flags & STF_FILE) {
      if (arrnam) {
        array.push_back(*file);
      } else if (hashnam) {
        hash.push_back("name");
        hash.push_back(*file);
      } else {
        s::printf("%s%s", file->c_str(), (flags & STF_PICK) ? " " : ":\n");
      }
    }
    auto emit = [&](int which) {
      str out = statprint(sb, file, which, flags, timefmt);
      if (arrnam) {
        array.push_back(s::move(out));
      } else if (hashnam) {
        hash.push_back(STATELTS[which]);
        hash.push_back(s::move(out));
      } else {
        s::printf("%s\n", out.c_str());
      }
    };
    if (iwhich > -1) {
      emit(iwhich);
    } else {
      for (int k = 0; k < ST_COUNT; ++k) emit(k);
    }
    if (o['f']) break;
    if (not arrnam and not hashnam and i + 1 < args.size() and not (flags & STF_PICK))
      s::putchar('\n');
  }
  if (arrnam and not ret and not set_array(*arrnam, s::move(array))) return 1;
  if (hashnam and not ret) {
    // sethparam: the parameter is made an association.
    if (auto h = ctx.params.lookup(*hashnam); not h or not h->get().is_hash()) {
      if (h and h->get().is_readonly()) {
        s::fprintf(stderr, "jfsh: read-only variable: %s\n", hashnam->c_str());
        return 1;
      }
      (void)ctx.params.unset(*hashnam);
      ctx.params.assign(*hashnam, umap<str, str>{}, PM_HASH);
    }
    if (not set_array(*hashnam, s::move(hash))) return 1;
  }
  return ret;
}

}  // namespace

void register_stat_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/stat", [](ModuleFeatures& out, ExecContext&) {
    out.builtins = {{"stat", bin_stat, BINF_NONE}, {"zstat", bin_stat, BINF_NONE}};
  });
}

}  // namespace jfsh
