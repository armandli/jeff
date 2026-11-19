#include <jfsh/modules/mapfile.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

// zsh/mapfile (M11): `$mapfile`, files as an associative array, as zsh's
// Src/Modules/mapfile.c.  Each element is read or written when it is
// used; nothing is cached.

namespace jfsh {

namespace {

opt<str> read_file(const str& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return s::nullopt;
  struct stat st{};
  if (::fstat(fd, &st) != 0 or S_ISDIR(st.st_mode)) {
    ::close(fd);
    return s::nullopt;
  }
  str out;
  char buf[8192];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) break;
    out.append(buf, static_cast<s::size_t>(n));
  }
  ::close(fd);
  return out;
}

void write_file(const str& path, strv text) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    mod_error("mapfile", path + ": " + s::strerror(errno));
    return;
  }
  while (not text.empty()) {
    ssize_t n = ::write(fd, text.data(), text.size());
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) break;
    text.remove_prefix(static_cast<s::size_t>(n));
  }
  ::close(fd);
}

// The keys: the regular files in the current directory.
umap<str, str> scan_cwd() {
  umap<str, str> h;
  DIR* d = ::opendir(".");
  if (not d) return h;
  while (struct dirent* e = ::readdir(d)) {
    str name = e->d_name;
    if (name == "." or name == "..") continue;
    struct stat st{};
    if (::stat(name.c_str(), &st) != 0 or not S_ISREG(st.st_mode)) continue;
    if (auto v = read_file(name)) h[name] = s::move(*v);
  }
  ::closedir(d);
  return h;
}

}  // namespace

void register_mapfile_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/mapfile", [](ModuleFeatures& out, ExecContext& ctx) {
    ExecContext* ctxp = &ctx;
    Param& p = ctx.params.assign("mapfile", umap<str, str>{}, PM_HASH | PM_SPECIAL);
    p.handlers = s::make_unique<ParamHandlers>();
    p.handlers->get_hash   = [] { return scan_cwd(); };
    p.handlers->get_elem   = [](const str& key) { return read_file(key); };
    p.handlers->set_elem   = [](const str& key, str value) { write_file(key, value); };
    p.handlers->unset_elem = [](const str& key) { (void)::unlink(key.c_str()); };
    // `mapfile=(file text …)` writes each file.
    p.handlers->set_array = [](vec<str> words) {
      for (s::size_t k = 0; k + 1 < words.size(); k += 2) write_file(words[k], words[k + 1]);
    };
    out.params.emplace_back("mapfile");
    out.on_unload = [ctxp] { drop_module_param(ctxp->params, "mapfile"); };
    out.feature_names = {"p:mapfile"};
  });
}

}  // namespace jfsh
