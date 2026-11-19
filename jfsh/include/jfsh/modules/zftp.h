#ifndef JFSH_MODULES_ZFTP_H
#define JFSH_MODULES_ZFTP_H
// jfsh/modules/zftp.h — the `zsh/zftp` module (M11).
//
// Contributes `zftp`, an FTP client with named sessions, and the
// $ZFTP_* parameters that describe the current one: open, login, params,
// cd, cdup, dir, ls, get, getat, put, putat, append, appendat, delete,
// mkdir, rmdir, rename, remote, local, type, ascii, binary, mode, quote,
// site, close, quit, test, session and rmsession.  Data connections are
// passive (PASV), as zsh's default $ZFTP_PREFS asks.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_zftp_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_ZFTP_H
