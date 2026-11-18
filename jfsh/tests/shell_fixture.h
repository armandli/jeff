#ifndef JFSH_TESTS_SHELL_FIXTURE_H
#define JFSH_TESTS_SHELL_FIXTURE_H
// Test helpers shared by the executor and expansion tests: an in-process
// shell whose stdout/stderr are captured, and a scratch directory.

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <jfsh/core/aliases.h>
#include <jfsh/core/jobs.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/signals.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/datetime.h>
#include <jfsh/modules/files.h>
#include <jfsh/modules/mathfunc.h>
#include <jfsh/modules/parameter.h>
#include <jfsh/modules/system.h>
#include <jfsh/modules/zutil.h>

namespace jfsh::test {

namespace sf = s::filesystem;

struct Run {
  int rc;
  str out;
  str err;
};

inline str slurp(s::FILE* f) {
  str s;
  s::rewind(f);
  char buf[4096];
  s::size_t n;
  while ((n = s::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  s::fclose(f);
  return s;
}

// A fresh shell state with $PATH exported from the test's environment.
struct Shell {
  ParamTable  params;
  OptionState opts;
  JobTable    jobs;
  ExecContext ctx{params, opts, jobs, Signals};
  Executor    exec{ctx};

  Shell() {
    const char* path = s::getenv("PATH");
    params.assign("PATH", str(path ? path : "/usr/bin:/bin"),
                  PM_SCALAR | PM_EXPORTED);
    // Register and auto-load the shipped modules so the in-process shell
    // sees zsh/datetime, zsh/mathfunc, zsh/parameter and zsh/zutil the
    // way the jfsh binary does.
    register_datetime_module (ctx.modules);
    register_mathfunc_module (ctx.modules);
    register_parameter_module(ctx.modules);
    register_zutil_module    (ctx.modules);
    register_system_module   (ctx.modules);
    register_files_module    (ctx.modules);
    (void)ctx.modules.load("zsh/datetime",  &ctx);
    (void)ctx.modules.load("zsh/mathfunc",  &ctx);
    (void)ctx.modules.load("zsh/parameter", &ctx);
    (void)ctx.modules.load("zsh/zutil",     &ctx);
    (void)ctx.modules.load("zsh/system",    &ctx);
    // zsh/files is registered but not loaded by default; tests that want
    // its builtins use `sh.run("zmodload zsh/files; …")`.
  }
  // Traps live in the process-wide Signals: don't let one test's traps
  // reach the next.
  ~Shell() { Signals.reset_traps(); }

  // Runs src with fds 1 and 2 captured.  Child processes inherit the
  // redirected fds, so their output is captured too.
  // Each run is like a `jfsh -c`: one that exited or aborted (an
  // expansion error ends a non-interactive shell) doesn't stop the next.
  Run run(strv src) { return capture(src, false); }
  // Runs src a line at a time, as a script file is.
  Run run_script(strv src) { return capture(src, true); }

  Run capture(strv src, bool by_line) {
    return call([&] { return exec.exec_string(src, by_line); });
  }

  // Runs fn with fds 1 and 2 captured, as run() does with a command.
  template <class Fn>
  Run call(Fn fn) {
    ctx.exiting  = false;
    ctx.aborting = false;
    s::fflush(nullptr);
    s::FILE* out = s::tmpfile();
    s::FILE* err = s::tmpfile();
    int saved_out = dup(STDOUT_FILENO);
    int saved_err = dup(STDERR_FILENO);
    dup2(fileno(out), STDOUT_FILENO);
    dup2(fileno(err), STDERR_FILENO);
    int rc = fn();
    s::fflush(nullptr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    return {rc, slurp(out), slurp(err)};
  }

  str get(strv name) {
    auto p = params.lookup(name);
    return (p and p->get().is_set()) ? p->get().scalar() : "<unset>";
  }
};

// A scratch directory removed when the test ends.
struct TempDir {
  sf::path path;

  TempDir() {
    str tmpl = (sf::temp_directory_path() / "jfsh_test_XXXXXX").string();
    path = mkdtemp(tmpl.data());
  }
  ~TempDir() { sf::remove_all(path); }

  sf::path file(strv name, strv body, sf::perms mode) {
    sf::path p = path / name;
    s::ofstream{p} << body;
    sf::permissions(p, mode);
    return p;
  }
};

}  // namespace jfsh::test

#endif  // JFSH_TESTS_SHELL_FIXTURE_H
