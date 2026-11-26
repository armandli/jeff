// zsh/system: a port of zsh 5.9's Src/Modules/system.c.  zsh's copyright
// and licence:
//
// Copyright (c) 1998-2003 Peter Stephenson
// All rights reserved.
//
// Permission is hereby granted, without written agreement and without
// license or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall Peter Stephenson or the Zsh Development
// Group be liable to any party for direct, indirect, special, incidental,
// or consequential damages arising out of the use of this software and
// its documentation, even if Peter Stephenson, and the Zsh
// Development Group have been advised of the possibility of such damage.
//
// Peter Stephenson and the Zsh Development Group specifically
// disclaim any warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose.  The
// software provided hereunder is on an "as is" basis, and Peter Stephenson
// and the Zsh Development Group have no obligation to provide maintenance,
// support, updates, enhancements, or modifications.

#include <jfsh/modules/system.h>

#include <fcntl.h>
#include <poll.h>
#include <strings.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <algorithm>
#include <memory>

#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

namespace jfsh {

namespace {

// zsh's sys_errnames: errno names by number from 1, as the system's
// errno.h has them (the first name a number has; those a platform hasn't
// are left out).
const vec<str>& errnames() {
  static const vec<str> names = [] {
    vec<str> v;
    auto put = [&](const char* name, int value) {
      if (value <= 0 or value > 4096) return;
      auto i = static_cast<s::size_t>(value - 1);
      if (v.size() <= i) v.resize(i + 1);
      if (v[i].empty()) v[i] = name;
    };
#ifdef EPERM
    put("EPERM", EPERM);
#endif
#ifdef ENOENT
    put("ENOENT", ENOENT);
#endif
#ifdef ESRCH
    put("ESRCH", ESRCH);
#endif
#ifdef EINTR
    put("EINTR", EINTR);
#endif
#ifdef EIO
    put("EIO", EIO);
#endif
#ifdef ENXIO
    put("ENXIO", ENXIO);
#endif
#ifdef E2BIG
    put("E2BIG", E2BIG);
#endif
#ifdef ENOEXEC
    put("ENOEXEC", ENOEXEC);
#endif
#ifdef EBADF
    put("EBADF", EBADF);
#endif
#ifdef ECHILD
    put("ECHILD", ECHILD);
#endif
#ifdef EDEADLK
    put("EDEADLK", EDEADLK);
#endif
#ifdef ENOMEM
    put("ENOMEM", ENOMEM);
#endif
#ifdef EACCES
    put("EACCES", EACCES);
#endif
#ifdef EFAULT
    put("EFAULT", EFAULT);
#endif
#ifdef ENOTBLK
    put("ENOTBLK", ENOTBLK);
#endif
#ifdef EBUSY
    put("EBUSY", EBUSY);
#endif
#ifdef EEXIST
    put("EEXIST", EEXIST);
#endif
#ifdef EXDEV
    put("EXDEV", EXDEV);
#endif
#ifdef ENODEV
    put("ENODEV", ENODEV);
#endif
#ifdef ENOTDIR
    put("ENOTDIR", ENOTDIR);
#endif
#ifdef EISDIR
    put("EISDIR", EISDIR);
#endif
#ifdef EINVAL
    put("EINVAL", EINVAL);
#endif
#ifdef ENFILE
    put("ENFILE", ENFILE);
#endif
#ifdef EMFILE
    put("EMFILE", EMFILE);
#endif
#ifdef ENOTTY
    put("ENOTTY", ENOTTY);
#endif
#ifdef ETXTBSY
    put("ETXTBSY", ETXTBSY);
#endif
#ifdef EFBIG
    put("EFBIG", EFBIG);
#endif
#ifdef ENOSPC
    put("ENOSPC", ENOSPC);
#endif
#ifdef ESPIPE
    put("ESPIPE", ESPIPE);
#endif
#ifdef EROFS
    put("EROFS", EROFS);
#endif
#ifdef EMLINK
    put("EMLINK", EMLINK);
#endif
#ifdef EPIPE
    put("EPIPE", EPIPE);
#endif
#ifdef EDOM
    put("EDOM", EDOM);
#endif
#ifdef ERANGE
    put("ERANGE", ERANGE);
#endif
#ifdef EAGAIN
    put("EAGAIN", EAGAIN);
#endif
#ifdef EINPROGRESS
    put("EINPROGRESS", EINPROGRESS);
#endif
#ifdef EALREADY
    put("EALREADY", EALREADY);
#endif
#ifdef ENOTSOCK
    put("ENOTSOCK", ENOTSOCK);
#endif
#ifdef EDESTADDRREQ
    put("EDESTADDRREQ", EDESTADDRREQ);
#endif
#ifdef EMSGSIZE
    put("EMSGSIZE", EMSGSIZE);
#endif
#ifdef EPROTOTYPE
    put("EPROTOTYPE", EPROTOTYPE);
#endif
#ifdef ENOPROTOOPT
    put("ENOPROTOOPT", ENOPROTOOPT);
#endif
#ifdef EPROTONOSUPPORT
    put("EPROTONOSUPPORT", EPROTONOSUPPORT);
#endif
#ifdef ESOCKTNOSUPPORT
    put("ESOCKTNOSUPPORT", ESOCKTNOSUPPORT);
#endif
#ifdef EOPNOTSUPP
    put("EOPNOTSUPP", EOPNOTSUPP);
#endif
#ifdef ENOTSUP
    put("ENOTSUP", ENOTSUP);
#endif
#ifdef EPFNOSUPPORT
    put("EPFNOSUPPORT", EPFNOSUPPORT);
#endif
#ifdef EAFNOSUPPORT
    put("EAFNOSUPPORT", EAFNOSUPPORT);
#endif
#ifdef EADDRINUSE
    put("EADDRINUSE", EADDRINUSE);
#endif
#ifdef EADDRNOTAVAIL
    put("EADDRNOTAVAIL", EADDRNOTAVAIL);
#endif
#ifdef ENETDOWN
    put("ENETDOWN", ENETDOWN);
#endif
#ifdef ENETUNREACH
    put("ENETUNREACH", ENETUNREACH);
#endif
#ifdef ENETRESET
    put("ENETRESET", ENETRESET);
#endif
#ifdef ECONNABORTED
    put("ECONNABORTED", ECONNABORTED);
#endif
#ifdef ECONNRESET
    put("ECONNRESET", ECONNRESET);
#endif
#ifdef ENOBUFS
    put("ENOBUFS", ENOBUFS);
#endif
#ifdef EISCONN
    put("EISCONN", EISCONN);
#endif
#ifdef ENOTCONN
    put("ENOTCONN", ENOTCONN);
#endif
#ifdef ESHUTDOWN
    put("ESHUTDOWN", ESHUTDOWN);
#endif
#ifdef ETOOMANYREFS
    put("ETOOMANYREFS", ETOOMANYREFS);
#endif
#ifdef ETIMEDOUT
    put("ETIMEDOUT", ETIMEDOUT);
#endif
#ifdef ECONNREFUSED
    put("ECONNREFUSED", ECONNREFUSED);
#endif
#ifdef ELOOP
    put("ELOOP", ELOOP);
#endif
#ifdef ENAMETOOLONG
    put("ENAMETOOLONG", ENAMETOOLONG);
#endif
#ifdef EHOSTDOWN
    put("EHOSTDOWN", EHOSTDOWN);
#endif
#ifdef EHOSTUNREACH
    put("EHOSTUNREACH", EHOSTUNREACH);
#endif
#ifdef ENOTEMPTY
    put("ENOTEMPTY", ENOTEMPTY);
#endif
#ifdef EPROCLIM
    put("EPROCLIM", EPROCLIM);
#endif
#ifdef EUSERS
    put("EUSERS", EUSERS);
#endif
#ifdef EDQUOT
    put("EDQUOT", EDQUOT);
#endif
#ifdef ESTALE
    put("ESTALE", ESTALE);
#endif
#ifdef EREMOTE
    put("EREMOTE", EREMOTE);
#endif
#ifdef EBADRPC
    put("EBADRPC", EBADRPC);
#endif
#ifdef ERPCMISMATCH
    put("ERPCMISMATCH", ERPCMISMATCH);
#endif
#ifdef EPROGUNAVAIL
    put("EPROGUNAVAIL", EPROGUNAVAIL);
#endif
#ifdef EPROGMISMATCH
    put("EPROGMISMATCH", EPROGMISMATCH);
#endif
#ifdef EPROCUNAVAIL
    put("EPROCUNAVAIL", EPROCUNAVAIL);
#endif
#ifdef ENOLCK
    put("ENOLCK", ENOLCK);
#endif
#ifdef ENOSYS
    put("ENOSYS", ENOSYS);
#endif
#ifdef EFTYPE
    put("EFTYPE", EFTYPE);
#endif
#ifdef EAUTH
    put("EAUTH", EAUTH);
#endif
#ifdef ENEEDAUTH
    put("ENEEDAUTH", ENEEDAUTH);
#endif
#ifdef EPWROFF
    put("EPWROFF", EPWROFF);
#endif
#ifdef EDEVERR
    put("EDEVERR", EDEVERR);
#endif
#ifdef EOVERFLOW
    put("EOVERFLOW", EOVERFLOW);
#endif
#ifdef EBADEXEC
    put("EBADEXEC", EBADEXEC);
#endif
#ifdef EBADARCH
    put("EBADARCH", EBADARCH);
#endif
#ifdef ESHLIBVERS
    put("ESHLIBVERS", ESHLIBVERS);
#endif
#ifdef EBADMACHO
    put("EBADMACHO", EBADMACHO);
#endif
#ifdef ECANCELED
    put("ECANCELED", ECANCELED);
#endif
#ifdef EIDRM
    put("EIDRM", EIDRM);
#endif
#ifdef ENOMSG
    put("ENOMSG", ENOMSG);
#endif
#ifdef EILSEQ
    put("EILSEQ", EILSEQ);
#endif
#ifdef ENOATTR
    put("ENOATTR", ENOATTR);
#endif
#ifdef EBADMSG
    put("EBADMSG", EBADMSG);
#endif
#ifdef EMULTIHOP
    put("EMULTIHOP", EMULTIHOP);
#endif
#ifdef ENODATA
    put("ENODATA", ENODATA);
#endif
#ifdef ENOLINK
    put("ENOLINK", ENOLINK);
#endif
#ifdef ENOSR
    put("ENOSR", ENOSR);
#endif
#ifdef ENOSTR
    put("ENOSTR", ENOSTR);
#endif
#ifdef EPROTO
    put("EPROTO", EPROTO);
#endif
#ifdef ETIME
    put("ETIME", ETIME);
#endif
#ifdef ENOPOLICY
    put("ENOPOLICY", ENOPOLICY);
#endif
#ifdef ENOTRECOVERABLE
    put("ENOTRECOVERABLE", ENOTRECOVERABLE);
#endif
#ifdef EOWNERDEAD
    put("EOWNERDEAD", EOWNERDEAD);
#endif
#ifdef EQFULL
    put("EQFULL", EQFULL);
#endif
#ifdef ENOTCAPABLE
    put("ENOTCAPABLE", ENOTCAPABLE);
#endif
#ifdef ECHRNG
    put("ECHRNG", ECHRNG);
#endif
#ifdef EL2NSYNC
    put("EL2NSYNC", EL2NSYNC);
#endif
#ifdef EL3HLT
    put("EL3HLT", EL3HLT);
#endif
#ifdef EL3RST
    put("EL3RST", EL3RST);
#endif
#ifdef ELNRNG
    put("ELNRNG", ELNRNG);
#endif
#ifdef EUNATCH
    put("EUNATCH", EUNATCH);
#endif
#ifdef ENOCSI
    put("ENOCSI", ENOCSI);
#endif
#ifdef EL2HLT
    put("EL2HLT", EL2HLT);
#endif
#ifdef EBADE
    put("EBADE", EBADE);
#endif
#ifdef EBADR
    put("EBADR", EBADR);
#endif
#ifdef EXFULL
    put("EXFULL", EXFULL);
#endif
#ifdef ENOANO
    put("ENOANO", ENOANO);
#endif
#ifdef EBADRQC
    put("EBADRQC", EBADRQC);
#endif
#ifdef EBADSLT
    put("EBADSLT", EBADSLT);
#endif
#ifdef EBFONT
    put("EBFONT", EBFONT);
#endif
#ifdef ENONET
    put("ENONET", ENONET);
#endif
#ifdef ENOPKG
    put("ENOPKG", ENOPKG);
#endif
#ifdef EADV
    put("EADV", EADV);
#endif
#ifdef ESRMNT
    put("ESRMNT", ESRMNT);
#endif
#ifdef ECOMM
    put("ECOMM", ECOMM);
#endif
#ifdef EDOTDOT
    put("EDOTDOT", EDOTDOT);
#endif
#ifdef ENOTUNIQ
    put("ENOTUNIQ", ENOTUNIQ);
#endif
#ifdef EBADFD
    put("EBADFD", EBADFD);
#endif
#ifdef EREMCHG
    put("EREMCHG", EREMCHG);
#endif
#ifdef ELIBACC
    put("ELIBACC", ELIBACC);
#endif
#ifdef ELIBBAD
    put("ELIBBAD", ELIBBAD);
#endif
#ifdef ELIBSCN
    put("ELIBSCN", ELIBSCN);
#endif
#ifdef ELIBMAX
    put("ELIBMAX", ELIBMAX);
#endif
#ifdef ELIBEXEC
    put("ELIBEXEC", ELIBEXEC);
#endif
#ifdef ERESTART
    put("ERESTART", ERESTART);
#endif
#ifdef ESTRPIPE
    put("ESTRPIPE", ESTRPIPE);
#endif
#ifdef EUCLEAN
    put("EUCLEAN", EUCLEAN);
#endif
#ifdef ENOTNAM
    put("ENOTNAM", ENOTNAM);
#endif
#ifdef ENAVAIL
    put("ENAVAIL", ENAVAIL);
#endif
#ifdef EISNAM
    put("EISNAM", EISNAM);
#endif
#ifdef EREMOTEIO
    put("EREMOTEIO", EREMOTEIO);
#endif
#ifdef ENOMEDIUM
    put("ENOMEDIUM", ENOMEDIUM);
#endif
#ifdef EMEDIUMTYPE
    put("EMEDIUMTYPE", EMEDIUMTYPE);
#endif
#ifdef ENOKEY
    put("ENOKEY", ENOKEY);
#endif
#ifdef EKEYEXPIRED
    put("EKEYEXPIRED", EKEYEXPIRED);
#endif
#ifdef EKEYREVOKED
    put("EKEYREVOKED", EKEYREVOKED);
#endif
#ifdef EKEYREJECTED
    put("EKEYREJECTED", EKEYREJECTED);
#endif
#ifdef ERFKILL
    put("ERFKILL", ERFKILL);
#endif
#ifdef EHWPOISON
    put("EHWPOISON", EHWPOISON);
#endif
    return v;
  }();
  return names;
}

// zsh's getposint.
int getposint(const str& instr, strv nam) {
  auto r = zstrtol(instr, 0, 10);
  if (r.end != instr.size() or r.value < 0) {
    mod_error(nam, "integer expected: " + instr);
    return -1;
  }
  return static_cast<int>(r.value);
}

bool interrupted(const ExecContext& ctx) {
  return ctx.aborting or ctx.returning or ctx.break_level;
}

// zsh's movefd: the descriptor moved to 10 or above.
int movefd(int fd) {
  if (fd < 0 or fd >= 10) return fd;
  int nfd = ::fcntl(fd, F_DUPFD, 10);
  ::close(fd);
  return nfd;
}

// sysread [-c countvar] [-i infd] [-o outfd] [-s bufsize] [-t timeout]
// [param]: 0 read, 1 bad arguments, 2 a read error, 3 a write error, 4 a
// timeout, 5 end of file.
int bin_sysread(vec<str>& args, ExecContext& ctx) {
  ZOps ops;
  int first = zsh_options(args, "c:i:o:s:t:", 0, 1, ops);
  if (first < 0) return 1;
  strv nam = args[0];
  int infd = 0, outfd = -1, bufsize = 8192;
  const str* countvar = nullptr;
  opt<str> outvar;
  if (ops.isset('i') and (infd = getposint(ops.arg('i'), nam)) < 0) return 1;
  if (ops.isset('o') and (outfd = getposint(ops.arg('o'), nam)) < 0) return 1;
  if (ops.isset('s') and (bufsize = getposint(ops.arg('s'), nam)) < 0) return 1;
  if (ops.isset('c')) {
    countvar = &ops.arg('c');
    if (not zsh_isident(*countvar)) return mod_error(nam, "not an identifier: " + *countvar);
  }
  if (static_cast<s::size_t>(first) < args.size()) {
    outvar = args[static_cast<s::size_t>(first)];
    if (not zsh_isident(*outvar)) return mod_error(nam, "not an identifier: " + *outvar);
  }
  vec<char> inbuf(static_cast<s::size_t>(bufsize > 0 ? bufsize : 1));
  if (ops.isset('t')) {
    auto to = math_eval(ops.arg('t'), ctx.params, math_options(ctx), &ctx.modules.math_funcs());
    if (not to) {
      s::fprintf(stderr, "jfsh: %s\n", to.error().message.c_str());
      return 1;
    }
    int to_int = to->is_float ? static_cast<int>(1000 * to->d) : 1000 * static_cast<int>(to->i);
    struct pollfd pfd{infd, POLLIN, 0};
    int ret;
    while ((ret = ::poll(&pfd, 1, to_int)) < 0)
      if (errno != EINTR or interrupted(ctx)) break;
    if (ret <= 0) return ret ? 2 : 4;
  }
  ssize_t count;
  while ((count = ::read(infd, inbuf.data(), static_cast<s::size_t>(bufsize))) < 0)
    if (errno != EINTR or interrupted(ctx)) break;
  if (countvar) (void)set_iparam(ctx, *countvar, count);
  if (count < 0) return 2;
  if (outfd >= 0) {
    if (not count) return 5;
    const char* p = inbuf.data();
    while (count > 0) {
      ssize_t ret = ::write(outfd, p, static_cast<s::size_t>(count));
      if (ret < 0) {
        if (errno == EINTR and not interrupted(ctx)) continue;
        if (outvar) (void)ctx.executor->assign_var(*outvar, str(p, static_cast<s::size_t>(count)));
        if (countvar) (void)set_iparam(ctx, *countvar, count);
        return 3;
      }
      p += ret;
      count -= ret;
    }
    return 0;
  }
  // Even when nothing was read.
  (void)ctx.executor->assign_var(outvar ? *outvar : str("REPLY"),
                                 str(inbuf.data(), static_cast<s::size_t>(count)));
  return count ? 0 : 5;
}

// syswrite [-c countvar] [-o outfd] data: 0 written, 1 bad arguments, 2 a
// write error.
int bin_syswrite(vec<str>& args, ExecContext& ctx) {
  ZOps ops;
  int first = zsh_options(args, "c:o:", 1, 1, ops);
  if (first < 0) return 1;
  strv nam = args[0];
  int outfd = 1;
  const str* countvar = nullptr;
  if (ops.isset('o') and (outfd = getposint(ops.arg('o'), nam)) < 0) return 1;
  if (ops.isset('c')) {
    countvar = &ops.arg('c');
    if (not zsh_isident(*countvar)) return mod_error(nam, "not an identifier: " + *countvar);
  }
  const str& data = args[static_cast<s::size_t>(first)];
  s::size_t done = 0;
  while (done < data.size()) {
    ssize_t n;
    while ((n = ::write(outfd, data.data() + done, data.size() - done)) < 0) {
      if (errno != EINTR or interrupted(ctx)) {
        if (countvar) (void)set_iparam(ctx, *countvar, static_cast<s::int64_t>(done));
        return 2;
      }
    }
    done += static_cast<s::size_t>(n);
  }
  if (countvar) (void)set_iparam(ctx, *countvar, static_cast<s::int64_t>(done));
  return 0;
}

struct OpenOpt { const char* name; int oflag; };

// zsh's openopts.
const vec<OpenOpt>& openopts() {
  static const vec<OpenOpt> table = [] {
    vec<OpenOpt> t{{"cloexec", O_CLOEXEC}, {"nofollow", O_NOFOLLOW}, {"sync", O_SYNC}};
#ifdef O_NOATIME
    t.push_back({"noatime", O_NOATIME});
#endif
    for (OpenOpt o : {OpenOpt{"nonblock", O_NONBLOCK}, OpenOpt{"excl", O_EXCL | O_CREAT},
                      OpenOpt{"creat", O_CREAT}, OpenOpt{"create", O_CREAT},
                      OpenOpt{"truncate", O_TRUNC}, OpenOpt{"trunc", O_TRUNC}})
      t.push_back(o);
    return t;
  }();
  return table;
}

// sysopen [-arw] [-m perms] [-o opts] -u fd|param file
int bin_sysopen(vec<str>& args, ExecContext& ctx) {
  ZOps ops;
  int first = zsh_options(args, "rwau:o:m:", 1, 1, ops);
  if (first < 0) return 1;
  strv nam = args[0];
  bool rd = ops.isset('r'), wr = ops.isset('w');
  int append = ops.isset('a') ? O_APPEND : 0;
  int flags = O_NOCTTY | append | ((append or wr) ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY);
  if (not ops.isset('u')) return mod_error(nam, "file descriptor not specified");
  const str& fdvar = ops.arg('u');
  int explicit_fd = -1;
  if (fdvar.size() == 1 and s::isdigit(static_cast<unsigned char>(fdvar[0])))
    explicit_fd = fdvar[0] - '0';
  else if (not zsh_isident(fdvar))
    return mod_error(nam, "not an identifier: " + fdvar);
  if (ops.isset('o')) {
    str opts = ops.arg('o');
    s::size_t at = 0;
    while (at != str::npos) {
      if (opts.size() - at >= 2 and ::strncasecmp(opts.c_str() + at, "O_", 2) == 0) at += 2;
      auto comma = opts.find(',', at);
      str word = opts.substr(at, comma == str::npos ? str::npos : comma - at);
      at = comma == str::npos ? str::npos : comma + 1;
      const auto& table = openopts();
      auto o = s::find_if(table.rbegin(), table.rend(), [&](const OpenOpt& e) {
        return ::strcasecmp(e.name, word.c_str()) == 0;
      });
      if (o == table.rend()) return mod_error(nam, "unsupported option: " + word + "\n");
      flags |= o->oflag;
    }
  }
  mode_t perms = 0666;
  if (ops.isset('m')) {
    const str& m = ops.arg('m');
    s::size_t n = 0;
    while (n < m.size() and m[n] >= '0' and m[n] <= '7') ++n;
    if (n < m.size() or n < 3) return mod_error(nam, "invalid mode " + m);
    perms = static_cast<mode_t>(zstrtol(m, 0, 8).value);
  }
  const str& file = args[static_cast<s::size_t>(first)];
  int fd = (flags & O_CREAT) ? ::open(file.c_str(), flags, perms) : ::open(file.c_str(), flags);
  if (fd == -1) return mod_error(nam, "can't open file " + file + ": " + errno_text(errno));
  int moved = fd;
  if (explicit_fd > -1) {
    if (fd != explicit_fd) {
      moved = ::dup2(fd, explicit_fd);
      ::close(fd);
    }
  } else {
    moved = movefd(fd);
  }
  if (moved == -1) return mod_error(nam, "can't open file " + file);
  if ((flags & O_CLOEXEC) and fd != moved) ::fcntl(moved, F_SETFD, FD_CLOEXEC);
  Executor::note_user_fd(moved);
  if (explicit_fd == -1 and not set_iparam(ctx, fdvar, moved)) ::close(moved);
  return 0;
}

// sysseek [-u fd] [-w start|end|current] offset: 0 done, 1 bad arguments,
// 2 a seek error.
int bin_sysseek(vec<str>& args, ExecContext& ctx) {
  ZOps ops;
  int first = zsh_options(args, "u:w:", 1, 1, ops);
  if (first < 0) return 1;
  strv nam = args[0];
  int w = SEEK_SET, fd = 0;
  if (ops.isset('u') and (fd = getposint(ops.arg('u'), nam)) < 0) return 1;
  if (ops.isset('w')) {
    const str& whence = ops.arg('w');
    if (::strcasecmp(whence.c_str(), "current") == 0 or whence == "1") w = SEEK_CUR;
    else if (::strcasecmp(whence.c_str(), "end") == 0 or whence == "2") w = SEEK_END;
    else if (::strcasecmp(whence.c_str(), "start") != 0 and whence != "0")
      return mod_error(nam, "unknown argument to -w: " + whence);
  }
  auto pos = math_eval(args[static_cast<s::size_t>(first)], ctx.params, math_options(ctx),
                       &ctx.modules.math_funcs());
  if (not pos) {
    s::fprintf(stderr, "jfsh: %s\n", pos.error().message.c_str());
    return 1;
  }
  return ::lseek(fd, static_cast<off_t>(pos->as_int()), w) == -1 ? 2 : 0;
}

// syserror [-e errvar] [-p prefix] [errno | errname]: 0 done, 1 bad
// arguments, 2 a name not known.
int bin_syserror(vec<str>& args, ExecContext& ctx) {
  ZOps ops;
  int first = zsh_options(args, "e:p:", 0, 1, ops);
  if (first < 0) return 1;
  strv nam = args[0];
  const str* errvar = nullptr;
  str pfx;
  if (ops.isset('e')) {
    errvar = &ops.arg('e');
    if (not zsh_isident(*errvar)) return mod_error(nam, "not an identifier: " + *errvar);
  }
  if (ops.isset('p')) pfx = ops.arg('p');
  int num = errno;
  if (static_cast<s::size_t>(first) < args.size()) {
    const str& a = args[static_cast<s::size_t>(first)];
    if (not a.empty() and a.find_first_not_of("0123456789") == str::npos) {
      num = s::atoi(a.c_str());
    } else {
      const auto& names = errnames();
      auto at = s::find(names.begin(), names.end(), a);
      if (a.empty() or at == names.end()) return 2;
      num = static_cast<int>(at - names.begin()) + 1;
    }
  }
  str msg = pfx + s::strerror(num);
  if (errvar) (void)ctx.executor->assign_var(*errvar, msg);
  else s::fprintf(stderr, "%s\n", msg.c_str());
  return 0;
}

// The descriptors zsystem flock holds (zsh's addlockfd / zcloselockfd).
struct LockFds {
  vec<int> fds;
};

int zsystem_flock(strv nam, vec<str> args, ExecContext& ctx, LockFds& locks) {
  bool cloexec = true, unlock = false, readlock = false;
  double timeout = -1;
  long timeout_interval = 1000000;
  opt<str> fdvar;
  s::size_t a = 0;
  auto num = [&](const str& text) {
    return math_eval(text, ctx.params, math_options(ctx), &ctx.modules.math_funcs());
  };
  while (a < args.size() and not args[a].empty() and args[a][0] == '-') {
    str word = args[a++];
    strv optptr = strv(word).substr(1);
    if (optptr.empty() or optptr == "-") break;
    for (s::size_t k = 0; k < optptr.size(); ++k) {
      char oc = optptr[k];
      auto optarg = [&](const char* what) -> opt<str> {
        if (k + 1 < optptr.size()) {
          str v(optptr.substr(k + 1));
          k = optptr.size() - 1;
          return v;
        }
        if (a >= args.size()) {
          mod_error(nam, str("flock: option ") + oc + " requires " + what);
          return s::nullopt;
        }
        return args[a++];
      };
      switch (oc) {
        case 'e': cloexec = false;

        break; case 'f': {
          if (k + 1 < optptr.size()) {
            fdvar = str(optptr.substr(k + 1));
            k = optptr.size() - 1;
          } else if (a < args.size()) {
            fdvar = args[a++];
          }
          if (not fdvar or not zsh_isident(*fdvar))
            return mod_error(nam, str("flock: option ") + oc + " requires a variable name");
        }

        break; case 'r': readlock = true;

        break; case 't': {
          auto v = optarg("a numeric timeout");
          if (not v) return 1;
          auto t = num(*v);
          if (not t) return 1;
          timeout = t->as_float();
          if (timeout > 1073741823.)
            return mod_error(nam, "flock: invalid timeout value: '" + *v + "'");
        }

        break; case 'i': {
          auto v = optarg("a numeric retry interval");
          if (not v) return 1;
          auto t = num(*v);
          if (not t) return 1;
          double us = s::ceil(t->as_float() * 1e6);
          if (us < 1 or us > 0.999 * static_cast<double>(LONG_MAX))
            return mod_error(nam, "flock: invalid interval value: '" + *v + "'");
          timeout_interval = static_cast<long>(us);
        }

        break; case 'u': unlock = true;

        break; default: return mod_error(nam, str("flock: unknown option: ") + oc);
      }
    }
  }
  if (a >= args.size()) return mod_error(nam, "flock: not enough arguments");
  if (a + 1 < args.size()) return mod_error(nam, "flock: too many arguments");
  const str& file = args[a];
  if (unlock) {
    auto v = num(file);
    int fd = v ? static_cast<int>(v->as_int()) : 0;
    auto it = s::find(locks.fds.begin(), locks.fds.end(), fd);
    if (it == locks.fds.end())
      return mod_error(nam, "flock: file descriptor " + s::to_string(fd)
                                + " not in use for locking");
    locks.fds.erase(it);
    ::close(fd);
    return 0;
  }
  int flags = (readlock ? O_RDONLY : O_RDWR) | O_NOCTTY;
  int fd = ::open(file.c_str(), flags);
  if (fd < 0)
    return mod_error(nam, "failed to open " + file + " for writing: " + errno_text(errno));
  fd = movefd(fd);
  if (fd == -1) return 1;
  if (cloexec) ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
  locks.fds.push_back(fd);
  auto drop = [&] {
    locks.fds.erase(s::find(locks.fds.begin(), locks.fds.end(), fd));
    ::close(fd);
  };
  struct flock lck{};
  lck.l_type   = readlock ? F_RDLCK : F_WRLCK;
  lck.l_whence = SEEK_SET;
  if (timeout > 0) {
    struct timespec now{}, end{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    double whole;
    end = now;
    end.tv_nsec += static_cast<long>(s::ceil(s::modf(timeout, &whole) * 1e9));
    end.tv_sec += static_cast<time_t>(whole);
    if (end.tv_nsec >= 1000000000L) {
      end.tv_nsec -= 1000000000L;
      end.tv_sec += 1;
    }
    while (::fcntl(fd, F_SETLK, &lck) < 0) {
      if (ctx.aborting) {
        drop();
        return 1;
      }
      if (errno != EINTR and errno != EACCES and errno != EAGAIN) {
        drop();
        return mod_error(nam, "failed to lock file " + file + ": " + errno_text(errno));
      }
      ::clock_gettime(CLOCK_MONOTONIC, &now);
      long remaining = static_cast<long>((end.tv_sec - now.tv_sec) * 1000000L
                                         + (end.tv_nsec - now.tv_nsec) / 1000);
      if (remaining <= 0) {
        drop();
        return 2;
      }
      if (remaining <= timeout_interval) timeout_interval = remaining;
      ::usleep(static_cast<useconds_t>(timeout_interval));
    }
  } else {
    while (::fcntl(fd, timeout == 0 ? F_SETLK : F_SETLKW, &lck) < 0) {
      if (ctx.aborting) {
        drop();
        return 1;
      }
      if (errno == EINTR) continue;
      int err = errno;
      drop();
      return mod_error(nam, "failed to lock file " + file + ": " + errno_text(err));
    }
  }
  if (fdvar) (void)set_iparam(ctx, *fdvar, fd);
  return 0;
}

int bin_zsystem(vec<str>& args, ExecContext& ctx, LockFds& locks) {
  ZOps ops;
  int first = zsh_options(args, nullptr, 1, -1, ops);
  if (first < 0) return 1;
  strv nam = args[0];
  vec<str> rest(args.begin() + first + 1, args.end());
  const str& sub = args[static_cast<s::size_t>(first)];
  if (sub == "flock") return zsystem_flock(nam, s::move(rest), ctx, locks);
  if (sub == "supports") {
    if (rest.empty()) {
      mod_error(nam, "supports: not enough arguments");
      return 255;
    }
    if (rest.size() > 1) {
      mod_error(nam, "supports: too many arguments");
      return 255;
    }
    return rest[0] == "supports" or rest[0] == "flock" ? 0 : 1;
  }
  return mod_error(nam, "unknown subcommand: " + sub);
}

}  // namespace

void register_system_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/system", [](ModuleFeatures& out, ExecContext& ctx) {
    auto locks = s::make_shared<LockFds>();
    out.builtins = {
      {"syserror", bin_syserror, BINF_NONE},
      {"sysread", bin_sysread, BINF_NONE},
      {"syswrite", bin_syswrite, BINF_NONE},
      {"sysopen", bin_sysopen, BINF_NONE},
      {"sysseek", bin_sysseek, BINF_NONE},
      {"zsystem", [locks](vec<str>& a, ExecContext& c) { return bin_zsystem(a, c, *locks); },
       BINF_NONE},
    };
    // systell(fd): where fd is.
    out.mathfuncs.push_back(MathFuncDef{"systell", [](const vec<MathNum>& argv) -> Result<MathNum> {
      int fd = argv[0].is_float ? static_cast<int>(argv[0].d) : static_cast<int>(argv[0].i);
      if (fd < 0)
        return s::unexpected(ShellError{ErrorKind::ExecError, "file descriptor out of range"});
      return MathNum::of(static_cast<s::int64_t>(::lseek(fd, 0, SEEK_CUR)));
    }, {}, 1, 1});
    constexpr s::uint32_t SPECIAL = PM_SPECIAL | PM_READONLY | PM_HIDE | PM_HIDEVAL;
    // $errnos: the errno names by number.
    Param& e = ctx.params.assign("errnos", vec<str>{}, PM_ARRAY | SPECIAL);
    e.handlers = s::make_unique<ParamHandlers>();
    e.handlers->get_array = [] { return errnames(); };
    // $sysparams: pid, ppid and procsubstpid.
    Param& sp = ctx.params.assign("sysparams", umap<str, str>{}, PM_HASH | SPECIAL);
    sp.handlers = s::make_unique<ParamHandlers>();
    auto value = [](const str& k) -> opt<str> {
      if (k == "pid") return s::to_string(::getpid());
      if (k == "ppid") return s::to_string(::getppid());
      if (k == "procsubstpid") return str("0");
      return s::nullopt;
    };
    sp.handlers->get_hash = [value] {
      umap<str, str> h;
      for (const char* k : {"pid", "ppid", "procsubstpid"}) h[k] = *value(k);
      return h;
    };
    sp.handlers->get_elem = value;
    out.params = {"errnos", "sysparams"};
    out.on_unload = [locks] {
      for (int fd : locks->fds) ::close(fd);
      locks->fds.clear();
    };
  });
}

}  // namespace jfsh
