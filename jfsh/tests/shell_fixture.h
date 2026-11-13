#ifndef JFSH_TESTS_SHELL_FIXTURE_H
#define JFSH_TESTS_SHELL_FIXTURE_H
// Test helpers shared by the executor and expansion tests: an in-process
// shell whose stdout/stderr are captured, and a scratch directory.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <unistd.h>

#include <jfsh/core/jobs.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/signals.h>
#include <jfsh/exec/executor.h>

namespace jfsh::test {

namespace fs = std::filesystem;

struct Run {
  int         rc;
  std::string out;
  std::string err;
};

inline std::string slurp(std::FILE* f) {
  std::string s;
  std::rewind(f);
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  std::fclose(f);
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
    const char* path = std::getenv("PATH");
    params.assign("PATH", std::string(path ? path : "/usr/bin:/bin"),
                  PM_SCALAR | PM_EXPORTED);
  }

  // Runs src with fds 1 and 2 captured.  Child processes inherit the
  // redirected fds, so their output is captured too.
  Run run(std::string_view src) {
    std::fflush(nullptr);
    std::FILE* out = std::tmpfile();
    std::FILE* err = std::tmpfile();
    int saved_out = dup(STDOUT_FILENO);
    int saved_err = dup(STDERR_FILENO);
    dup2(fileno(out), STDOUT_FILENO);
    dup2(fileno(err), STDERR_FILENO);
    int rc = exec.exec_string(src);
    std::fflush(nullptr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    return {rc, slurp(out), slurp(err)};
  }

  std::string get(std::string_view name) {
    auto p = params.lookup(name);
    return (p and p->get().is_set()) ? p->get().scalar() : "<unset>";
  }
};

// A scratch directory removed when the test ends.
struct TempDir {
  fs::path path;

  TempDir() {
    std::string tmpl = (fs::temp_directory_path() / "jfsh_test_XXXXXX").string();
    path = mkdtemp(tmpl.data());
  }
  ~TempDir() { fs::remove_all(path); }

  fs::path file(std::string_view name, std::string_view body, fs::perms mode) {
    fs::path p = path / name;
    std::ofstream{p} << body;
    fs::permissions(p, mode);
    return p;
  }
};

}  // namespace jfsh::test

#endif  // JFSH_TESTS_SHELL_FIXTURE_H
