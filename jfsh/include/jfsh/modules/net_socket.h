#ifndef JFSH_MODULES_NET_SOCKET_H
#define JFSH_MODULES_NET_SOCKET_H
// jfsh/modules/net_socket.h — the `zsh/net/socket` module (M11 Phase 11a).
//
// Contributes `zsocket`: a shell binding for AF_UNIX socket(2) /
// connect(2) / listen(2) / accept(2).
//
//   zsocket -l path    — listen on a UNIX-domain socket; $REPLY = fd
//   zsocket    path    — connect to a UNIX-domain socket;  $REPLY = fd
//   zsocket -a fd      — accept on fd; $REPLY = client fd
//   zsocket -d fd      — close fd
//
// `zsh/socket` is registered as an alias for `zsh/net/socket` (zsh's
// historical name) so `zmodload zsh/socket` resolves the same module.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_net_socket_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_NET_SOCKET_H
