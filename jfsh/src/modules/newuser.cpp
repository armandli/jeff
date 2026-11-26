#include <jfsh/modules/newuser.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

// zsh/newuser (M11): the first-login helper, after zsh's newuser module
// and its zsh-newuser-install function, much reduced: a menu on the
// terminal that writes a starter ~/.jfshrc with the prompt picked.

namespace jfsh {

namespace {

struct Theme {
  const char* name;
  const char* ps1;
  const char* rps1;
};

constexpr Theme kThemes[] = {
  {"minimal",  "%~ %# ",                                   ""},
  {"classic",  "%n@%m %~ %# ",                             ""},
  {"colorful", "%F{green}%n@%m%f %F{blue}%~%f %# ",        "%(?..%F{red}%?%f)"},
  {"two-line", "%F{cyan}%~%f\n%(?.%F{green}.%F{red})%#%f ", "%T"},
};

str rc_dir(ExecContext& ctx) {
  if (str z = param_or(ctx.params, "ZDOTDIR"); not z.empty()) return z;
  if (str h = param_or(ctx.params, "HOME"); not h.empty()) return h;
  if (struct passwd* pw = ::getpwuid(::getuid()); pw and pw->pw_dir) return pw->pw_dir;
  return ".";
}

// A line from stdin, without its newline; null at EOF.
opt<str> ask(strv prompt) {
  s::printf("%.*s", static_cast<int>(prompt.size()), prompt.data());
  s::fflush(stdout);
  str line;
  char c;
  for (;;) {
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) return line.empty() ? opt<str>{} : opt<str>{line};
    if (c == '\n') return line;
    line += c;
  }
}

// Single-quotes `v` for the rc file.
str sq(strv v) {
  str out = "'";
  for (char c : v) out += c == '\'' ? str("'\\''") : str(1, c);
  return out + "'";
}

bool write_rc(const str& path, const str& text) {
  s::FILE* f = s::fopen(path.c_str(), "w");
  if (not f) {
    mod_error("zsh-newuser-install", path + ": " + s::strerror(errno));
    return false;
  }
  s::fputs(text.c_str(), f);
  s::fclose(f);
  return true;
}

str starter_rc(const Theme& t) {
  str rc =
    "# ~/.jfshrc, written by zsh-newuser-install.\n"
    "\n"
    "HISTFILE=~/.jfsh_history\n"
    "HISTSIZE=10000\n"
    "SAVEHIST=10000\n"
    "setopt share_history hist_ignore_dups extended_glob\n"
    "bindkey -e\n"
    "\n";
  rc += str("# Prompt theme: ") + t.name + "\n";
  rc += "PS1=" + sq(t.ps1) + "\n";
  if (*t.rps1) rc += "RPS1=" + sq(t.rps1) + "\n";
  return rc;
}

int builtin_newuser_install(vec<str>& args, ExecContext& ctx) {
  bool force = args.size() > 1 and args[1] == "-f";
  if (not force and not newuser_needed(ctx)) return 0;
  str path = rc_dir(ctx) + "/.jfshrc";
  s::printf(
    "This is the jfsh new-user setup.  You have no startup files yet\n"
    "(%s and the like), so jfsh can write one for you.\n\n"
    "(q)  Quit and do nothing.  This will run again next time.\n"
    "(0)  Exit, creating an empty ~/.jfshrc so this doesn't run again.\n"
    "(1)  Pick a prompt and write a starter ~/.jfshrc.\n\n", path.c_str());
  auto choice = ask("--- Type one of the keys in parentheses --- ");
  if (not choice or *choice == "q" or choice->empty()) return 0;
  if (*choice == "0")
    return write_rc(path, "# Created by zsh-newuser-install.\n") ? 0 : 1;
  if (*choice != "1") return mod_error("zsh-newuser-install", "unknown choice: " + *choice);
  s::printf("\nPrompt themes:\n");
  int n = 0;
  for (const auto& t : kThemes) s::printf("(%d)  %-9s %s\n", ++n, t.name, t.ps1);
  auto pick = ask("--- Type the number of a theme --- ");
  int k = pick ? s::atoi(pick->c_str()) : 0;
  if (k < 1 or k > n) return mod_error("zsh-newuser-install", "no such theme");
  const Theme& t = kThemes[k - 1];
  if (not write_rc(path, starter_rc(t))) return 1;
  s::printf("Wrote %s with the %s prompt.\n", path.c_str(), t.name);
  (void)ctx.params.set_scalar("PS1", t.ps1);
  if (*t.rps1) (void)ctx.params.set_scalar("RPS1", t.rps1);
  return 0;
}

}  // namespace

bool newuser_needed(ExecContext& ctx) {
  str dir = rc_dir(ctx);
  // A directory that isn't there gets no files written to it.
  if (struct stat st{}; ::stat(dir.c_str(), &st) != 0 or not S_ISDIR(st.st_mode)) return false;
  for (const char* f : {".jfshenv", ".jfsh_profile", ".jfshrc", ".jfsh_login"}) {
    struct stat st{};
    if (::stat((dir + "/" + f).c_str(), &st) == 0) return false;
  }
  return true;
}

void register_newuser_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/newuser", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"zsh-newuser-install", builtin_newuser_install, BINF_NONE, 0});
  });
}

}  // namespace jfsh
