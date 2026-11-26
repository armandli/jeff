#ifndef JFSH_TESTS_SHELL_FIXTURE_H
#define JFSH_TESTS_SHELL_FIXTURE_H
// Test helpers shared by the executor and expansion tests: an in-process
// shell whose stdout/stderr are captured, and a scratch directory.

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <jfsh/core/aliases.h>
#include <jfsh/core/jobs.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/signals.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/module.h>

namespace jfsh::test {

// A UTF-8 LC_CTYPE for one test, for towlower(), MB_CUR_MAX and friends.
struct Utf8Ctype {
  Utf8Ctype() : saved{s::setlocale(LC_CTYPE, nullptr)} {
    for (const char* name : {"en_US.UTF-8", "C.UTF-8", "en_US.utf8"})
      if (s::setlocale(LC_CTYPE, name)) { ok = true; break; }
  }
  ~Utf8Ctype() { s::setlocale(LC_CTYPE, saved.c_str()); }

  str  saved;
  bool ok = false;
};

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
    // The shipped modules as the jfsh binary has them: zsh/main loaded,
    // the rest autoloadable as in zsh, or loaded with zmodload.
    setup_shipped_modules(ctx);
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
    ctx.lookup_error = false;
    s::fflush(nullptr);
    s::FILE* out = s::tmpfile();
    s::FILE* err = s::tmpfile();
    int saved_in  = dup(STDIN_FILENO);
    int saved_out = dup(STDOUT_FILENO);
    int saved_err = dup(STDERR_FILENO);
    // Never the test runner's stdin: a command that reads it (`cat` with
    // nothing to read) would wait for ever.
    if (int null = open("/dev/null", O_RDONLY); null >= 0) {
      dup2(null, STDIN_FILENO);
      close(null);
    }
    dup2(fileno(out), STDOUT_FILENO);
    dup2(fileno(err), STDERR_FILENO);
    int rc = fn();
    s::fflush(nullptr);
    dup2(saved_in, STDIN_FILENO);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_in);
    close(saved_out);
    close(saved_err);
    return {rc, slurp(out), slurp(err)};
  }

  str get(strv name) {
    auto p = params.lookup(name);
    return (p and p->get().is_set()) ? p->get().scalar() : "<unset>";
  }
};

// Runs fn on a thread whose stack is `bytes`, so tests of the stack-depth
// limits do not depend on `ulimit -s`.  An exception from fn is rethrown
// here; a thread that cannot be made fails the test.
template <typename Fn>
void on_stack(s::size_t bytes, Fn fn) {
  struct Job {
    Fn&                fn;
    s::exception_ptr   error;
  } job{fn, nullptr};
  auto run = [](void* p) -> void* {
    auto* j = static_cast<Job*>(p);
    try { j->fn(); } catch (...) { j->error = s::current_exception(); }
    return nullptr;
  };
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, bytes);
  pthread_t t;
  int rc = pthread_create(&t, &attr, run, &job);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    ADD_FAILURE() << "pthread_create failed: " << rc;
    return;
  }
  pthread_join(t, nullptr);
  if (job.error) s::rethrow_exception(job.error);
}

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
