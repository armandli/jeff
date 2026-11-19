#include <jfsh/modules/clone.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

// zsh/clone (M11): `clone tty`, as zsh's Src/Modules/clone.c.  Both
// processes return from the builtin and go on with the same code: the
// parent with $! set to the child, the child on the new terminal.

namespace jfsh {

namespace {

int builtin_clone(vec<str>& args, ExecContext& ctx) {
  if (args.size() != 2) return mod_error("clone", "terminal name required");
  const str& tty = args[1];
  int fd = ::open(tty.c_str(), O_RDWR | O_NOCTTY);
  if (fd < 0) return mod_error("clone", tty + ": " + s::strerror(errno));
  s::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fd);
    return mod_error("clone", str("fork failed: ") + s::strerror(errno));
  }
  if (pid > 0) {
    ::close(fd);
    ctx.last_bg_pid = pid;
    return 0;
  }
  // The child: a new session whose controlling terminal is `tty`.
  ::setsid();
#ifdef TIOCSCTTY
  ::ioctl(fd, TIOCSCTTY, 0);
#endif
  for (int target = 0; target < 3; ++target) ::dup2(fd, target);
  if (fd > 2) ::close(fd);
  ctx.shell_pid = ::getpid();
  (void)ctx.params.set_scalar("TTY", tty);
  return 0;
}

}  // namespace

void register_clone_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/clone", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"clone", builtin_clone, BINF_NONE, 1, 1});
    out.feature_names = {"b:clone"};
  });
}

}  // namespace jfsh
