#ifndef JFSH_MODULES_NET_TCP_H
#define JFSH_MODULES_NET_TCP_H
// jfsh/modules/net_tcp.h — the `zsh/net/tcp` module (M11).
//
// Contributes `ztcp`: TCP sessions over socket(2) / connect(2) /
// listen(2) / accept(2), each tracked so `ztcp` alone can list them.
//
//   ztcp [-v] [-d fd] host [port]   — connect; $REPLY = fd
//   ztcp -l [-v] [-d fd] port       — listen on port; $REPLY = fd
//   ztcp -a [-tv] [-d fd] listenfd  — accept; $REPLY = fd (-t: don't wait)
//   ztcp -c [-f] [fd]               — close one session, or all of them
//   ztcp [-L]                       — list sessions (-L: for scripts)

#include <jfsh/modules/module.h>

namespace jfsh {

void register_net_tcp_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_NET_TCP_H
