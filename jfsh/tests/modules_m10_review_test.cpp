// Tests from the M10 review (branch jfsh-m10-review): the module framework
// (a port of zsh 5.9's Src/module.c) and the core modules, each check run in
// a fresh shell, its expected output and status taken from `zsh -f -c`.

#include <string>

#include <gtest/gtest.h>

#include <jfsh/exec/loop.h>
#include <shell_fixture.h>

namespace jfsh::test {

namespace {

// zsh's own text after "failed to load module `x': " is dlopen's message,
// which names its module path.
str load_errors_cut(str err) {
  for (s::size_t at = 0; (at = err.find("failed to load module `", at)) != str::npos;) {
    auto q = err.find('\'', at + 23);
    auto nl = err.find('\n', q);
    if (q == str::npos or nl == str::npos) break;
    err.erase(q + 1, nl - q - 1);
    at = q;
  }
  return err;
}

struct Expect {
  const char* cmd;
  const char* out;
  const char* err;
  int         rc;
};

// `init`: the shell's parameters as the jfsh binary starts with them
// (init_shell_params: $PWD, the tied specials, …).
void check(const Expect& e, bool init = false) {
  Shell sh;
  if (init) init_shell_params(sh.ctx);
  auto r = sh.run(e.cmd);
  EXPECT_EQ(r.out, e.out) << e.cmd;
  EXPECT_EQ(load_errors_cut(r.err), e.err) << e.cmd;
  EXPECT_EQ(r.rc, e.rc) << e.cmd;
}

}  // namespace

// As zsh -f: zsh/main loaded, zsh's autoload marks and dependencies.
TEST(ModulesM10Review, StartupStateAsZshF) {
  const Expect cases[] = {
    {"zmodload; zmodload -L; zmodload -e zsh/main; echo $?; zmodload -e "
     "zsh/zutil; echo $?",
     "zsh/main\nzmodload zsh/main\n0\n1\n",
     "", 0},
    {"zmodload -Ld",
     "zmodload -d zsh/compctl zsh/complete zsh/zle\n"
     "zmodload -d zsh/complete zsh/zle\n"
     "zmodload -d zsh/complist zsh/complete zsh/zle\n"
     "zmodload -d zsh/computil zsh/complete zsh/zle\n"
     "zmodload -d zsh/zleparameter zsh/zle\n"
     "zmodload -d zsh/zutil zsh/complete\n",
     "", 0},
    {"zmodload -LFa zsh/zutil; zmodload -LFa zsh/zle; zmodload -LFa "
     "zsh/complete",
     "zmodload -aF zsh/zutil b:zformat b:zparseopts b:zregexparse b:zstyle\n"
     "zmodload -aF zsh/zle b:bindkey b:vared b:zle\n"
     "zmodload -aF zsh/complete b:compadd b:compset c:after c:between "
     "c:prefix c:suffix\n",
     "", 0},
    {"zmodload -Lac",
     "zmodload -ac zsh/complete after\nzmodload -ac zsh/complete between\n"
     "zmodload -ac zsh/complete suffix\nzmodload -ac zsh/complete prefix\n",
     "", 0},
    {"zmodload -ab | grep zsh/zutil",
     "zformat (zsh/zutil)\nzparseopts (zsh/zutil)\nzregexparse (zsh/zutil)\n"
     "zstyle (zsh/zutil)\n",
     "", 0},
    {"zmodload -ap | grep -c zsh/parameter",
     "33\n",
     "", 0},
    {"zmodload -lF zsh/main; echo $?",
     "1\n",
     "jfsh: zmodload: module `zsh/main' does not support features\n", 0},
  };
  for (const auto& e : cases) check(e);
}

// A builtin's mark loads its module with that feature, and the modules it needs.
TEST(ModulesM10Review, AutoloadBringsOneFeature) {
  const Expect cases[] = {
    {"zstyle; zmodload; zmodload -lF zsh/zutil",
     "zsh/complete\nzsh/main\nzsh/zle\nzsh/zutil\n-b:zformat\n"
     "-b:zparseopts\n-b:zregexparse\n+b:zstyle\n",
     "", 0},
    {"zformat -f x %a a:1; zmodload -LF zsh/zutil; echo "
     "$builtins[zparseopts] $builtins[zformat]",
     "zmodload -F zsh/zutil b:zformat undefined defined\n",
     "", 0},
    {"echo ${+functions}; zmodload; zmodload -LF zsh/parameter",
     "1\nzsh/main\nzsh/parameter\nzmodload -F zsh/parameter p:functions ",
     "", 0},
    {"zmodload -ab zsh/datetime strftime; strftime -s y %Y 0; zmodload -LF "
     "zsh/datetime",
     "zmodload -F zsh/datetime b:strftime p:EPOCHSECONDS p:EPOCHREALTIME "
     "p:epochtime\n",
     "", 0},
    {"zmodload -aF zsh/datetime b:strftime; strftime -s y %Y 0; zmodload "
     "-LF zsh/datetime; zmodload -LFa zsh/datetime",
     "zmodload -F zsh/datetime b:strftime zmodload -aF zsh/datetime "
     "b:strftime\n",
     "", 0},
    {"zmodload -af zsh/mathfunc cos; echo $(( cos(0) )); zmodload -LF "
     "zsh/mathfunc",
     "1.\nzmodload -F zsh/mathfunc f:cos ",
     "", 0},
    {"zmodload -ap zsh/datetime EPOCHSECONDS; [[ $EPOCHSECONDS = <-> ]] && "
     "zmodload -LF zsh/datetime",
     "zmodload -F zsh/datetime b:strftime p:EPOCHSECONDS p:EPOCHREALTIME "
     "p:epochtime\n",
     "", 0},
    {"echo $(( sqrt(4) )); echo ${+EPOCHSECONDS}",
     "",
     "jfsh: unknown function: sqrt\n", 1},
  };
  for (const auto& e : cases) check(e);
}

// A mark whose module doesn't load.
TEST(ModulesM10Review, FailedAutoloads) {
  const Expect cases[] = {
    {"zmodload -ab zsh/foo bar; bar; echo $?; whence -w bar",
     "1\nbar: none\n",
     "jfsh: failed to load module `zsh/foo'\n", 1},
    {"zmodload -ap zsh/foo foovar; echo ${+foovar} $foovar",
     "1 zsh/foo\n",
     "jfsh: failed to load module `zsh/foo'\n"
     "jfsh: failed to load module `zsh/foo'\n", 0},
    {"zmodload -aF zsh/datetime b:nope; echo $?; zmodload zsh/datetime; "
     "echo $?; zmodload -LFa zsh/datetime",
     "0\n0\nzmodload -aF zsh/datetime\n",
     "jfsh: module `zsh/datetime' has no such feature: `b:nope': autoload "
     "cancelled\n", 0},
    {"zmodload -ap zsh/example exnosuch; echo $exnosuch; echo after $?",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; x=$exnosuch; echo after $?",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; echo ${+exnosuch}; echo after",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; (echo $exnosuch); echo after $?",
     "The example module has now been set up.\nafter 1\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -af zsh/example nofn; echo $(( nofn(1) )); echo after $?",
     "The example module has now been set up.\n"
     "Thank you for using the example module.  Have a nice day.\n",
     "jfsh: module `zsh/example' has no such feature: `f:nofn': autoload "
     "cancelled\njfsh: autoloading module zsh/example failed to define math "
     "function: nofn\n", 1},
    {"zmodload -ap zsh/example exnosuch; (( exnosuch + 1 )); echo after $?",
     "The example module has now been set up.\nafter 2\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; let exnosuch; echo after $?; let "
     "exnosuch 1; echo after $?",
     "The example module has now been set up.\nafter 1\nafter 0\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; typeset exnosuch=3; echo rc $?",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; export exnosuch",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; while [[ -z $exnosuch ]]; do echo "
     "in; break; done; echo after",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/x XX; export XX; echo $?; XX=1; echo $? $XX",
     "0\n0 1\n",
     "jfsh: failed to load module `zsh/x'\n"
     "jfsh: failed to load module `zsh/x'\njfsh: failed to load module `1'\n", 0},
    {"zmodload -ap zsh/example exnosuch; echo ${exnosuch[1]} x; echo after "
     "$?",
     "The example module has now been set up.\nx\nafter 0\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; case $exnosuch in *) echo c;; "
     "esac; echo after",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; cat <<< $exnosuch; echo after $?",
     "The example module has now been set up.\nafter 1\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; : <<< $exnosuch; echo after $?",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; { cat; } <<< $exnosuch; echo after "
     "$?",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; unset 'exnosuch[1+]'; echo rc $?",
     "rc 1\n",
     "jfsh: bad math expression: operand expected at end of string\n", 0},
    {"zmodload -ap zsh/example exnosuch; [[ -z $exnosuch ]]",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; true; [[ -z $exnosuch ]]",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; if [[ -z $exnosuch ]]; then echo "
     "y; fi",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; while [[ -n $exnosuch ]]; do "
     "break; done",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; while true $exnosuch; do echo "
     "loop; break; done",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; until [[ -z $exnosuch ]]; do "
     "break; done",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; false; while [[ -n $exnosuch ]]; "
     "do :; done",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; a=(1); while [[ -z "
     "$exnosuch${a[1]} ]]; do break; done",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; a=(1); /bin/echo hi > "
     "$exnosuch${a[1]}; echo after $?",
     "The example module has now been set up.\nafter 1\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; x=7; (( x = exnosuch )); echo "
     "after $? x=$x",
     "The example module has now been set up.\nafter 2 x=7\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; x=7; (( x = exnosuch, y = 3 )); "
     "echo after $? x=$x y=$y",
     "The example module has now been set up.\nafter 2 x=7 y=\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; x=7; let \"x = 1 + exnosuch\"; "
     "echo after $? x=$x",
     "The example module has now been set up.\nafter 1 x=7\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; let \"y=2\" \"x = exnosuch\"; echo "
     "after $? $x $y",
     "The example module has now been set up.\nafter 1 2\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; (( exnosuch++ )); echo after $? "
     "$exnosuch",
     "The example module has now been set up.\nafter 2 1\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; x=$exnosuch /bin/echo hi; echo "
     "after $?",
     "The example module has now been set up.\nafter 1\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; x=$exnosuch true; echo after $?",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; y=$(exit 3)$exnosuch",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; command /bin/echo hi > $exnosuch; "
     "echo after $?",
     "The example module has now been set up.\nafter 1\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; > $exnosuch; echo after $?",
     "The example module has now been set up.\nafter 1\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; exnosuch[1]=5; echo rc $? $exnosuch",
     "The example module has now been set up.\nrc 0 5\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; exnosuch=5; echo rc $?",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; (exit 3); case $exnosuch in *) "
     "echo c;; esac",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; [[ -z $exnosuch ]] && echo yes",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; false; [[ -z $exnosuch ]] && echo "
     "yes",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; ! [[ -n $exnosuch ]]",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; select i in $exnosuch; do break; "
     "done",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; read exnosuch <<< val; echo r $?",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; (( $exnosuch y = 5 )); echo $? y=$y",
     "The example module has now been set up.\n2 y=\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; (( y = 5 $exnosuch )); echo $? y=$y",
     "The example module has now been set up.\n2 y=\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; (( y = ${exnosuch}5 )); echo $? "
     "y=$y",
     "The example module has now been set up.\n2 y=\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; [[ ${exnosuch}x = x ]]",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; [[ -n ${exnosuch}x ]]",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; [[ -z ${exnosuch}x ]]",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; [[ x${exnosuch} = x ]]",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; [[ \"${exnosuch}x\" == x ]]",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; echo ${exnosuch:?boom}",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; echo ${exnosuch:-def}",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; : ${exnosuch=5}",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; command -v echo > $exnosuch; echo "
     "after $?",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; setopt xtrace; x=$exnosuch "
     "/bin/echo hi",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; (exit 3); [[ -z $exnosuch ]]",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; (exit 3); echo $exnosuch",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 3},
    {"zmodload -ap zsh/example exnosuch; (exit 3); y=$exnosuch",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; (exit 3); while [[ -n $exnosuch "
     "]]; do :; done",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"command -v echo > ${nox?}; echo after $?",
     "",
     "jfsh: nox: parameter not set\n", 1},
    {"command -p -v echo > ${nox?}; echo after $?",
     "",
     "jfsh: nox: parameter not set\n", 1},
    {"setopt posixbuiltins; command echo > ${nox?}; echo after $?",
     "",
     "jfsh: nox: parameter not set\n", 1},
    {"setopt posixbuiltins; f(){ :; }; command f > ${nox?}; echo after $?",
     "after 1\n",
     "jfsh: nox: parameter not set\n", 0},
    {"command /bin/echo hi > ${nox?}; echo after $?",
     "after 1\n",
     "jfsh: nox: parameter not set\n", 0},
    {"echo x; /bin/echo a b > ${nox?}; echo \"[$_]\"",
     "x\n[b]\n",
     "jfsh: nox: parameter not set\n", 0},
    {"setopt shnullcmd; > ${x:?b}; echo after $?",
     "",
     "jfsh: x: b\n", 1},
    {"NULLCMD=:; > ${x:?b}; echo after $?",
     "",
     "jfsh: x: b\n", 1},
    {"READNULLCMD=:; < ${x:?b}; echo after $?",
     "",
     "jfsh: x: b\n", 1},
    {"noglob > ${x:?b}; echo after $?",
     "",
     "jfsh: redirection with no command\n", 1},
    {"command > ${x:?b}; echo after $?",
     "",
     "jfsh: redirection with no command\n", 1},
    {"NULLCMD=; > ${x:?b}; echo after $?",
     "",
     "jfsh: redirection with no command\n", 1},
    {"setopt cshnullcmd; > ${x:?b}; echo after $?",
     "",
     "jfsh: redirection with no command\n", 1},
    {"> ${x:?b}; echo after $?",
     "after 1\n",
     "jfsh: x: b\n", 0},
    {"f(){ :; }; NULLCMD=f; > ${x:?b}; echo after $?",
     "",
     "jfsh: x: b\n", 1},
    {"zmodload -ap zsh/example exnosuch; setopt shnullcmd; > $exnosuch; "
     "echo after $?",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; command > $exnosuch; echo after $?",
     "",
     "jfsh: redirection with no command\n", 1},
    {"zmodload -ap zsh/example exnosuch; echo nosuchdir/*${exnosuch} "
     "~nosuchuser${exnosuch}",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; echo =nosuchcmd${exnosuch}",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"zmodload -ap zsh/example exnosuch; y=~nosuchuser${exnosuch}",
     "The example module has now been set up.\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 1},
    {"(d=$(mktemp -d) && cd $d && trap \"cd / && rm -rf $d\" EXIT && "
     "command noglob > f$(echo X); echo *)",
     "fX\n",
     "jfsh: command not found: noglob\n", 0},
    {"command - > ${x?}; echo after $?",
     "after 1\n",
     "jfsh: x: parameter not set\n", 0},
    {"command noglob > ${nope?u}; echo after $?",
     "after 1\n",
     "jfsh: nope: u\n", 0},
    {"(d=$(mktemp -d) && cd $d && trap \"cd / && rm -rf $d\" EXIT && setopt "
     "posixbuiltins; command noglob > f$(echo X); echo r $?; echo *)",
     "",
     "jfsh: redirection with no command\n", 1},
    {"zmodload -ap zsh/example exnosuch; command exec > $exnosuch; echo "
     "after $?",
     "The example module has now been set up.\nafter 1\n",
     "jfsh: module `zsh/example' has no such feature: `p:exnosuch': "
     "autoload cancelled\njfsh: autoloading module zsh/example failed to "
     "define parameter: exnosuch\n", 0},
    {"zmodload -ap zsh/example exnosuch; builtin nosuch > $exnosuch; echo "
     "after $?",
     "after 1\n",
     "jfsh: no such builtin: nosuch\n", 0},
    {"builtin /bin/echo > ${nox?}; echo after $?",
     "after 1\n",
     "jfsh: no such builtin: /bin/echo\n", 0},
    {"setopt posixbuiltins; command builtin echo hi > ${nox?}; echo after $?",
     "",
     "jfsh: nox: parameter not set\n", 1},
    {"setopt posixbuiltins; noglob command builtin echo hi > ${nox?}; echo "
     "after $?",
     "",
     "jfsh: nox: parameter not set\n", 1},
    {"setopt posixbuiltins; command builtin nosuch > ${nox?}; echo after $?",
     "after 1\n",
     "jfsh: no such builtin: nosuch\n", 0},
  };
  for (const auto& e : cases) check(e);
}

// typeset on a module's mark: the name alone, -r and +x leave it; a value or -x loads.
TEST(ModulesM10Review, TypesetLeavesMarks) {
  const Expect cases[] = {
    {"zmodload -ap zsh/x XX; typeset XX; echo $?; zmodload -ap | grep XX",
     "XX\n0\nXX (zsh/x)\n",
     "", 0},
    {"zmodload -ap zsh/x XX; readonly XX; echo $?; typeset -g XX; echo $?; "
     "zmodload -ap | grep XX",
     "0\n0\nXX (zsh/x)\n",
     "", 0},
    {"zmodload -ap zsh/example exint; typeset exint; typeset -r exint; "
     "typeset +x exint; zmodload",
     "exint\nzsh/main\n",
     "", 0},
    {"zmodload -ap zsh/example exint; typeset -x exint; echo $?; zmodload",
     "The example module has now been set up.\n0\nzsh/example\nzsh/main\n",
     "", 0},
    {"zmodload -ap zsh/example exint; typeset exint=3; echo $? $exint",
     "The example module has now been set up.\n0 3\n",
     "", 0},
    {"f(){ local EPOCHSECONDS=1; echo $EPOCHSECONDS; }; zmodload -ap "
     "zsh/datetime EPOCHSECONDS; f; zmodload",
     "1\nzsh/main\n",
     "", 0},
    {"f(){ typeset -g EPOCHSECONDS; }; zmodload -ap zsh/datetime "
     "EPOCHSECONDS; f; echo $?; zmodload",
     "0\nzsh/main\n",
     "", 0},
    {"zmodload -ap zsh/example exarr; unset 'exarr[1]'; echo $?; zmodload",
     "0\nzsh/main\n",
     "", 0},
    {"for h in commands options aliases functions; do unset \"${h}[zzz]\"; "
     "echo $h $?; done; zmodload",
     "commands 1\noptions 1\naliases 1\nfunctions 1\nzsh/main\n",
     "jfsh: commands: assignment to invalid subscript range\n"
     "jfsh: options: assignment to invalid subscript range\n"
     "jfsh: aliases: assignment to invalid subscript range\n"
     "jfsh: functions: assignment to invalid subscript range\n", 0},
  };
  for (const auto& e : cases) check(e);
}

// Unsetting either side of $module_path or $fpath unsets both; an assignment makes the pair again.
TEST(ModulesM10Review, TiedSpecialsUnsetTogether) {
  const Expect cases[] = {
    {"unset module_path; echo ${(t)MODULE_PATH}x; module_path=(/a); print "
     "$module_path $MODULE_PATH ${(t)module_path} ${(t)MODULE_PATH}",
     "x\n/a /a array-tied-special scalar-tied-special\n",
     "", 0},
    {"unset MODULE_PATH; print ${(t)module_path}x ${(t)MODULE_PATH}x; "
     "MODULE_PATH=/b:/c; print $module_path ${(t)module_path} "
     "${(t)MODULE_PATH}",
     "x x\n/b /c array-tied-special scalar-tied-special\n",
     "", 0},
    {"unset fpath; fpath=(/a); print $fpath $FPATH ${(t)fpath}",
     "/a /a array-tied-special\n",
     "", 0},
    {"unset FPATH; print -r ${(t)fpath}x; FPATH=/q:/r; print $fpath "
     "${(t)fpath} ${(t)FPATH}",
     "x\n/q /r array-tied-special scalar-tied-special\n",
     "", 0},
    {"unset path; echo ${(t)PATH}x ${+PATH} ${+path}; path=(/bin); echo "
     "$PATH ${(t)path} ${(t)PATH}",
     "x 0 0\n/bin array-tied-special scalar-tied-export-special\n",
     "", 0},
    {"unset path; path[2]=/bin; echo $PATH; unset PATH; PATH+=/usr/bin; "
     "echo $PATH ${#path}",
     ":/bin\n/usr/bin 1\n",
     "", 0},
    {"unset FPATH; export FPATH=/q; env | grep ^FPATH; unset MODULE_PATH; "
     "export MODULE_PATH=/m; env | grep ^MODULE_PATH",
     "FPATH=/q\nMODULE_PATH=/m\n",
     "", 0},
    {"export CDPATH=/a; unset cdpath; export CDPATH=/c; env | grep ^CDPATH; "
     "export FPATH; unset fpath; env | grep -c ^FPATH",
     "CDPATH=/c\n0\n",
     "", 1},
    {"unset fpath; export FPATH=/q; sh -c 'echo $FPATH'",
     "/q\n",
     "", 0},
    {"typeset -T FOO foo; foo=(a b); unset FOO; foo=(c d); echo $FOO; "
     "typeset -T BAR bar; unset bar; BAR=x:y; echo $bar",
     "\n\n",
     "", 0},
    {"typeset -T FOO foo; unset FOO; typeset -p foo FOO; echo $?",
     "1\n",
     "jfsh: typeset: no such variable: foo\n"
     "jfsh: typeset: no such variable: FOO\n", 0},
    {"readonly fpath; unset FPATH; echo $?",
     "",
     "jfsh: read-only variable: fpath\n", 0},
    {"readonly MODULE_PATH; unset module_path; echo $?",
     "",
     "jfsh: read-only variable: MODULE_PATH\n", 0},
    {"fpath=(/g); f(){ local -h fpath=(/q); echo $FPATH; }; f; typeset -T "
     "FOO foo; foo=(a); g(){ local foo=(x y); echo $FOO; }; g",
     "/g\na\n",
     "", 0},
  };
  for (const auto& e : cases) check(e, true);
}

// bye, where, unhash and ttyctl, as zsh's.
TEST(ModulesM10Review, CoreBuiltinsBecomeZsh) {
  const Expect cases[] = {
    {"bye 3",
     "",
     "", 3},
    {"where echo; where -p echo",
     "echo: shell built-in command\n/bin/echo\n/bin/echo\n/bin/echo\n"
     "/bin/echo\n",
     "", 0},
    {"ttyctl -f; echo $?; ttyctl -u; echo $?; ttyctl x; echo $?",
     "0\n0\n1\n",
     "jfsh: ttyctl: too many arguments\n", 0},
    {"unhash; echo $?; unhash nosuch; echo $?; hash foo=/bin/echo; unhash "
     "foo; hash | grep -c foo",
     "1\n1\n0\n",
     "jfsh: unhash: not enough arguments\n"
     "jfsh: unhash: no such hash table element: nosuch\n", 1},
    {"alias a=b; unhash -a a; alias a; echo $?; alias -s c=cat; unhash -s "
     "c; alias -s; f(){ :; }; unhash -f f; whence -w f",
     "1\nf: none\n",
     "", 1},
    {"unhash -d x; echo $?; hash foo=/bin/echo; unhash -m 'f*'; echo $?; "
     "unhash -m 'zz*'; echo $?",
     "1\n0\n1\n",
     "jfsh: unhash: no such hash table element: x\n", 0},
    {"echo ${+builtins[bye]} ${+builtins[where]} ${+builtins[unhash]} "
     "${+builtins[ttyctl]} ${+builtins[-]}",
     "1 1 1 1 1\n",
     "", 0},
    {"ttyctl; ttyctl -f; ttyctl; ttyctl -u; ttyctl; ttyctl -fu; ttyctl",
     "tty is not frozen\ntty is frozen\ntty is not frozen\ntty is frozen\n",
     "", 0},
    {"alias a1=x; unhash -am 'zz*'; echo $?; unalias -m 'zz*'; echo $?; "
     "unalias -s -m 'zz*'; echo $?",
     "1\n1\n1\n",
     "", 0},
  };
  for (const auto& e : cases) check(e);
}

// zmodload -F: -l, -L, -e, -m, -P and turning features on and off.
TEST(ModulesM10Review, FeaturesListedAndToggled) {
  const Expect cases[] = {
    {"zmodload zsh/datetime; zmodload -lF zsh/datetime; zmodload -LF "
     "zsh/datetime; zmodload -lLF zsh/datetime",
     "+b:strftime\n+p:EPOCHSECONDS\n+p:EPOCHREALTIME\n+p:epochtime\n"
     "zmodload -F zsh/datetime b:strftime p:EPOCHSECONDS p:EPOCHREALTIME "
     "p:epochtime\nzmodload -F zsh/datetime +b:strftime +p:EPOCHSECONDS "
     "+p:EPOCHREALTIME +p:epochtime\n",
     "", 0},
    {"zmodload zsh/datetime; zmodload -lF zsh/datetime +b:strftime "
     "-p:epochtime; zmodload -mlF zsh/datetime 'p:*'",
     "+p:EPOCHSECONDS\n+p:EPOCHREALTIME\n+p:epochtime\n",
     "", 0},
    {"zmodload zsh/datetime; zmodload -eF zsh/datetime b:strftime; echo $?; "
     "zmodload -eF zsh/datetime -b:strftime; echo $?; zmodload -eF "
     "zsh/datetime b:nope; echo $?",
     "0\n1\n1\n",
     "", 0},
    {"zmodload zsh/datetime; zmodload -lF -P arr zsh/datetime; echo $arr; "
     "zmodload -LF -P arr zsh/datetime; echo $arr",
     "+b:strftime +p:EPOCHSECONDS +p:EPOCHREALTIME +p:epochtime\n"
     "b:strftime p:EPOCHSECONDS p:EPOCHREALTIME p:epochtime\n",
     "", 0},
    {"zmodload -F zsh/datetime b:strftime; echo ${+EPOCHSECONDS}; zmodload "
     "-F zsh/datetime +p:EPOCHSECONDS; zmodload -LF zsh/datetime",
     "0\nzmodload -F zsh/datetime b:strftime p:EPOCHSECONDS ",
     "", 0},
    {"zmodload zsh/datetime; zmodload -F zsh/datetime -p:EPOCHSECONDS; echo "
     "${+EPOCHSECONDS}; zmodload -F zsh/datetime +p:EPOCHSECONDS; echo "
     "${+EPOCHSECONDS}",
     "0\n1\n",
     "", 0},
    {"zmodload -mF zsh/datetime '-p:*'; zmodload -lF zsh/datetime",
     "-b:strftime\n-p:EPOCHSECONDS\n-p:EPOCHREALTIME\n-p:epochtime\n",
     "", 0},
    {"zmodload -F zsh/datetime b:nope; echo $?; zmodload -F zsh/main b:x; "
     "echo $?",
     "1\n1\n",
     "jfsh: module `zsh/datetime' has no such feature: `b:nope'\n"
     "jfsh: module `zsh/main' does not support features\n", 0},
    {"zmodload -lF zsh/datetime; echo $?",
     "1\n",
     "jfsh: zmodload: module `zsh/datetime' is not yet loaded\n", 0},
  };
  for (const auto& e : cases) check(e);
}

// zmodload -A, -R and -d, and a module others need.
TEST(ModulesM10Review, AliasesAndDependencies) {
  const Expect cases[] = {
    {"zmodload -A foo=zsh/datetime; zmodload -A; zmodload -LA; zmodload -e "
     "foo; echo $?; zmodload foo; zmodload -e foo; echo $?; zmodload",
     "foo -> zsh/datetime\nzmodload -A foo=zsh/datetime\n1\n0\n"
     "zsh/datetime\nzsh/main\n",
     "", 0},
    {"zmodload -A foo=bar; zmodload -A bar=foo; echo $?; zmodload -A "
     "foo=foo; echo $?",
     "1\n1\n",
     "jfsh: zmodload: module alias would refer to itself: bar\n"
     "jfsh: zmodload: module alias would refer to itself: foo\n", 0},
    {"zmodload -R; echo $?; zmodload -R nope; echo $?; zmodload -R foo=bar; "
     "echo $?; zmodload -R zsh/main; echo $?",
     "1\n1\n1\n1\n",
     "jfsh: zmodload: no module alias to remove\n"
     "jfsh: zmodload: no such module alias: nope\n"
     "jfsh: zmodload: bad syntax for removing module alias: foo\n"
     "jfsh: zmodload: module is not an alias: zsh/main\n", 0},
    {"zmodload -d zsh/foo zsh/bar zsh/baz; zmodload -d zsh/foo; zmodload "
     "-ud zsh/foo zsh/bar; zmodload -d zsh/foo",
     "zsh/foo: zsh/bar zsh/baz\nzsh/foo: zsh/baz\n",
     "", 0},
    {"zmodload zsh/zutil; zmodload -u zsh/complete; echo $?; zmodload -u "
     "zsh/zutil zsh/complete zsh/zle; echo $?; zmodload",
     "1\n0\nzsh/main\n",
     "jfsh: zmodload: module zsh/complete is in use by another module and "
     "cannot be unloaded\n", 0},
    {"zmodload -d zsh/datetime zsh/stat; zmodload zsh/datetime; zmodload; "
     "zmodload -u zsh/stat; echo $?",
     "zsh/datetime\nzsh/main\nzsh/stat\n1\n",
     "jfsh: zmodload: module zsh/stat is in use by another module and "
     "cannot be unloaded\n", 0},
  };
  for (const auto& e : cases) check(e);
}

// bin_zmodload's checks and messages.
TEST(ModulesM10Review, ErrorsAsBinZmodload) {
  const Expect cases[] = {
    {"zmodload -b x; echo $?",
     "1\n",
     "jfsh: zmodload: -b, -c, -f, and -p must be combined with -a or -u\n", 0},
    {"zmodload -Fb x; echo $?",
     "1\n",
     "jfsh: zmodload: -b, -c, -f, and -p must be combined with -a or -u\n", 0},
    {"zmodload -Aa x=y; echo $?",
     "1\n",
     "jfsh: zmodload: illegal flags combined with -A or -R\n", 0},
    {"zmodload -da x; echo $?",
     "1\n",
     "jfsh: zmodload: -d cannot be combined with -a\n", 0},
    {"zmodload -u; echo $?",
     "1\n",
     "jfsh: zmodload: what do you want to unload?\n", 0},
    {"zmodload -ei x; echo $?",
     "1\n",
     "jfsh: zmodload: -e cannot be combined with other options\n", 0},
    {"zmodload -l; echo $?",
     "1\n",
     "jfsh: zmodload: -l is only allowed with -F\n", 0},
    {"zmodload -abc zsh/x y; echo $?",
     "1\n",
     "jfsh: zmodload: use only one of -b, -c, or -p\n", 0},
    {"zmodload -F; echo $?",
     "1\n",
     "jfsh: zmodload: -F requires a module name\n", 0},
    {"zmodload -aF zsh/datetime bad; echo $?",
     "1\n",
     "jfsh: zmodload: bad format for autoloadable feature: `bad'\n", 0},
    {"zmodload -aF zsh/datetime x:y; echo $?",
     "1\n",
     "jfsh: zmodload: bad autoloadable feature type: `x'\n", 0},
    {"zmodload -aF zsh/datetime b:a/b; echo $?",
     "1\n",
     "jfsh: zmodload: a/b: `/' is illegal in a builtin\n", 0},
    {"zmodload -amF zsh/datetime b:x; echo $?",
     "1\n",
     "jfsh: zmodload: -m cannot be used with -a\n", 0},
    {"zmodload -ab zsh/foo echo; echo $?",
     "1\n",
     "jfsh: zmodload: failed to add builtin `echo'\n", 0},
    {"zmodload -ub nope; echo $?",
     "1\n",
     "jfsh: zmodload: nope: no such builtin\n", 0},
    {"zmodload -iub nope; echo $?",
     "0\n",
     "", 0},
    {"zmodload zsh/datetime; zmodload -u zsh/nonexist; echo $?; zmodload "
     "-iu zsh/nonexist; echo $?",
     "1\n0\n",
     "jfsh: zmodload: no such module zsh/nonexist\n", 0},
    {"zmodload 'a b'; echo after",
     "",
     "jfsh: invalid module name `a b'\n", 1},
  };
  for (const auto& e : cases) check(e);
}

// checkaddparam: a parameter already there keeps its place.
TEST(ModulesM10Review, ModuleParametersClash) {
  const Expect cases[] = {
    {"EPOCHSECONDS=1; zmodload zsh/datetime; echo $? $EPOCHSECONDS; "
     "zmodload -LF zsh/datetime",
     "2 1\nzmodload -F zsh/datetime b:strftime p:EPOCHREALTIME p:epochtime\n",
     "jfsh: Can't add module parameter `EPOCHSECONDS': parameter already "
     "exists\njfsh: zsh/datetime: error when adding parameter "
     "`EPOCHSECONDS'\n", 0},
    {"f() { local x; zmodload -ap zsh/foo x; echo $?; }; f",
     "1\n",
     "jfsh: Can't add module parameter `x': local parameter exists\n", 0},
    {"zmodload -ap zsh/foo PATH; echo $?; zmodload -iap zsh/foo PATH; echo "
     "$?",
     "1\n0\n",
     "jfsh: Can't add module parameter `PATH': parameter already exists\n", 0},
    {"zmodload zsh/example >/dev/null; unset exstr; zmodload -u "
     "zsh/example; echo $?; zmodload | grep -c example",
     "1\n1\n",
     "jfsh: zsh/example: parameter `exstr' already deleted\n", 0},
  };
  for (const auto& e : cases) check(e);
}

// $builtins, $modules, whence and enable/disable see the marks.
TEST(ModulesM10Review, BuiltinsModulesAndLookup) {
  const Expect cases[] = {
    {"echo $builtins[zstyle] $builtins[echo] \"[$builtins[strftime]]\"; "
     "zmodload zsh/datetime; echo $builtins[strftime]",
     "undefined defined []\ndefined\n",
     "", 0},
    {"echo $modules[zsh/main] $modules[zsh/zutil] \"[$modules[zsh/nope]]\"; "
     "zmodload -A foo=zsh/main; echo $modules[foo]",
     "loaded autoloaded []\nalias:zsh/main\n",
     "", 0},
    {"whence -w zstyle bindkey sched strftime; whence -v zstyle; zmodload",
     "zstyle: builtin\nbindkey: builtin\nsched: builtin\nstrftime: none\n"
     "zstyle is a shell builtin\nzsh/main\n",
     "", 0},
    {"disable zstyle; echo \"[$builtins[zstyle]]\" $dis_builtins[zstyle]; "
     "zstyle; echo $?; enable zstyle; zstyle; echo $?",
     "[] undefined\n127\n0\n",
     "jfsh: command not found: zstyle\n", 0},
    {"zmodload -F zsh/files b:zf_rm; whence -w rm zf_rm",
     "rm: command\nzf_rm: builtin\n",
     "", 0},
    {"disable sched; whence -w sched; zmodload zsh/datetime; disable "
     "strftime; whence -w strftime; disable zstyle; whence -w zstyle",
     "sched: none\nstrftime: none\nzstyle: none\n",
     "", 1},
    {"disable -m 'sch*'; whence -w sched; echo $?",
     "sched: none\n1\n",
     "", 0},
    {"f(){ :; }; disable -f f; whence -w f; type f; which f; disable echo; "
     "whence -w echo; whence -a echo",
     "f: none\nf not found\nf not found\necho: command\n/bin/echo\n"
     "/bin/echo\n",
     "", 0},
    {"zmodload zsh/zutil; disable zstyle; zmodload -u zsh/zutil; zmodload "
     "zsh/zutil; whence -w zstyle",
     "zstyle: builtin\n",
     "", 0},
    {"disable zstyle; zmodload zsh/zutil; whence -w zstyle",
     "zstyle: builtin\n",
     "", 0},
    {"zmodload zsh/zutil; disable zstyle; zmodload -F zsh/zutil -b:zstyle "
     "+b:zstyle; whence -w zstyle",
     "zstyle: none\n",
     "", 1},
    {"zmodload zsh/zutil; zstyle -e :x s 'zmodload -u zsh/zutil; "
     "reply=(v)'; zstyle -s :x s r; echo $r $?; zmodload | grep -c zutil",
     "v 0\n0\n",
     "", 1},
  };
  for (const auto& e : cases) check(e);
}

// zsh/parameter's tables that change the shell's.
TEST(ModulesM10Review, ParameterTablesAssignable) {
  const Expect cases[] = {
    {"functions[f]='echo hi; echo $1'; f arg; functions f",
     "hi\narg\nf () {\n\techo hi\n\techo $1\n}\n",
     "", 0},
    {"f() { :; }; unset 'functions[f]' 2>/dev/null; zmodload zsh/parameter; "
     "unset 'functions[f]'; whence -w f",
     "f: none\n",
     "", 1},
    {"functions=(a 'echo A' b 'echo B'); a; b",
     "A\nB\n",
     "", 0},
    {"functions[f]='if'; echo $?",
     "0\n",
     "jfsh:1: parse error near `if'\njfsh: invalid function definition\n", 0},
    {"aliases[x]='echo X'; alias x; galiases[Y]=y; alias Y; "
     "saliases[c]=cat; alias -s",
     "x='echo X'\nY=y\nc=cat\n",
     "", 0},
    {"options[kshglob]=on; echo $options[kshglob]; options[kshglob]=maybe; "
     "options[nonsense]=on; options[noglob]=on; echo $options[glob]",
     "on\noff\n",
     "jfsh: invalid value: maybe\njfsh: no such option: nonsense\n", 0},
    {"commands[foo]=/bin/echo; foo hi; echo $commands[foo]",
     "hi\n/bin/echo\n",
     "", 0},
    {"(cd / && dirstack=(/a /b); dirs; unset dirstack; echo ${+dirstack}; "
     "dirs)",
     "/ /a /b\n0\n/\n",
     "", 0},
    {"f() { :; }; disable -f f; echo \"[$functions[f]]\" "
     "\"[$dis_functions[f]]\"",
     "[] [\t:]\n",
     "", 0},
    {"alias q=1; aliases=(x 'echo X'); alias",
     "x='echo X'\n",
     "", 0},
    {"alias -g G=1; galiases=(H 2); alias -g; alias -s c=cat; saliases=(d "
     "dd); alias -s; alias | grep -c .",
     "H=2\nd=dd\n3\n",
     "", 0},
    {"commands=(foo /bin/echo); hash | wc -l; foo hi",
     "       1\nhi\n",
     "", 0},
    {"hash foo=/bin/ls; commands=(bar /bin/echo); hash | grep -c .; echo "
     "${commands[foo]}x",
     "2\n/bin/lsx\n",
     "", 0},
    {"alias a=b; aliases=(); alias | wc -l; set -A aliases; alias | wc -l",
     "       3\n       3\n",
     "", 0},
    {"alias -g a=b; galiases=(); alias -g | wc -l; alias -s a=b; "
     "saliases=(); alias -s | wc -l",
     "       1\n       1\n",
     "", 0},
    {"zmodload zsh/parameter; dirstack=x; echo ${(t)dirstack} $dirstack; "
     "dirstack+=y; echo $dirstack",
     "array-hide-hideval-special x\nx y\n",
     "", 0},
    {"zmodload zsh/parameter; aliases=x; echo rc $?",
     "",
     "jfsh: aliases: attempt to set slice of associative array\n", 1},
    {"zmodload zsh/parameter; options=x; echo rc $?",
     "",
     "jfsh: options: attempt to set slice of associative array\n", 1},
    {"zmodload zsh/parameter; options+=(globdots on); echo rc $? "
     "$options[globdots]",
     "rc 0 on\n",
     "", 0},
  };
  for (const auto& e : cases) check(e);
}

// The read-only views.
TEST(ModulesM10Review, ParameterViews) {
  const Expect cases[] = {
    {"x=1; typeset -a a; export Y=1; typeset -i i; typeset -A h; readonly "
     "r=2; echo $parameters[x] $parameters[a] $parameters[Y] $parameters[i] "
     "$parameters[h] $parameters[r]",
     "scalar array scalar-export integer association scalar-readonly\n",
     "", 0},
    {"echo $parameters[0] $parameters[ARGC] \"[$parameters[nonexistent]]\" "
     "${+parameters[nonexistent]}",
     "scalar-special integer-readonly-special [] 0\n",
     "", 0},
    {"echo $reswords; echo $patchars; echo ${#dis_reswords}",
     "if export declare function else float end do typeset then integer { "
     "select readonly coproc } ! case [[ repeat done for while time esac "
     "until local fi nocorrect foreach elif\n"
     "| ~ ( ? * [ < ^ # ?( *( +( !( \\!( @(\n0\n",
     "", 0},
    {"echo $options[braceexpand] $options[dotglob] ${+options[nonsense]}",
     "on off 0\n",
     "", 0},
    {"f() { :; }; echo \"[$functions_source[nope]]\" "
     "${+functions_source[nope]}",
     "[] 1\n",
     "", 0},
    {"echo ${(t)parameters} ${(t)builtins} ${(t)reswords} ${(t)dirstack} "
     "${(t)history} ${(t)funcstack}",
     "association-readonly-hide-hideval-special "
     "association-readonly-hide-hideval-special "
     "array-readonly-hide-hideval-special array-hide-hideval-special "
     "association-readonly-hide-hideval-special "
     "array-readonly-hide-hideval-special\n",
     "", 0},
    {"f() { g; }; g() { echo $funcstack; echo ${#functrace} "
     "${#funcsourcetrace} ${#funcfiletrace}; }; f",
     "g f\n2 2 2\n",
     "", 0},
  };
  for (const auto& e : cases) check(e);
}

// PM_HIDE: a local of a module's special is a plain parameter.
TEST(ModulesM10Review, ModuleSpecialsHideUnderLocal) {
  const Expect cases[] = {
    {"f() { local functions; echo ${(t)functions}; local -a reswords; "
     "reswords=(a); echo $reswords; }; f; echo ${#reswords}",
     "scalar-local\na\n31\n",
     "", 0},
    {"typeset -A functions; echo $?",
     "",
     "jfsh: typeset: functions: can't change type of autoloaded parameter\n", 1},
    {"typeset + | grep 'functions$'; typeset -p functions; echo $?; set | "
     "grep '^commands'",
     "undefined dis_functions\nundefined functions\n0\ncommands\n",
     "", 0},
  };
  for (const auto& e : cases) check(e);
}

// zstyle: setting, lookups, weights and listings.
TEST(ModulesM10Review, ZstyleAsBinZstyle) {
  const Expect cases[] = {
    {"zstyle ':a:b' s 1; zstyle ':a:*' s 2; zstyle ':*:b' s 3; zstyle '*' s "
     "4; zstyle ':a:b:c' s 5; zstyle -L",
     "zstyle :a:b:c s 5\nzstyle :a:b s 1\nzstyle ':a:*' s 2\n"
     "zstyle ':*:b' s 3\nzstyle '*' s 4\n",
     "", 0},
    {"zstyle ':a:*' s v w; zstyle ':a:b' s x; zstyle ':*' t y; zstyle",
     "s\n        :a:b x\n        :a:* v w\nt\n        :* y\n",
     "", 0},
    {"zstyle ':a:*' s 'a b' \"c'd\" ''; zstyle -L; zstyle -s :a:x s out -; "
     "echo $out",
     "zstyle ':a:*' s 'a b' 'c'\\''d' ''\na b-c'd-\n",
     "", 0},
    {"zstyle -e ':a:*' s 'reply=(x y)'; zstyle -L; zstyle -a :a:b s arr; "
     "echo $arr; zstyle -e ':b' s 'if'; echo $?",
     "zstyle -e ':a:*' s 'reply=(x y)'\nx y\n1\n",
     "jfsh:1: parse error near `if'\n", 0},
    {"zstyle ':a:*' b yes; zstyle -b :a:b b o; echo $? $o; zstyle -b :a:b "
     "nope o; echo $? $o",
     "0 yes\n1 no\n",
     "", 0},
    {"zstyle ':a:*' t a b c; zstyle -t :a:b t b; echo $?; zstyle -t :a:b t "
     "x; echo $?; zstyle -t :a:b nope; echo $?; zstyle -T :a:b nope; echo $?",
     "0\n1\n2\n0\n",
     "", 0},
    {"zstyle ':a:*' m foo bar; zstyle -m :a:b m 'b*'; echo $?; zstyle -m "
     ":a:b m 'x*'; echo $?",
     "0\n1\n",
     "", 0},
    {"zstyle ':a:*' s v; zstyle ':b:*' s w; zstyle ':a:*' t x; zstyle -g "
     "arr; echo ${(o)arr}; zstyle -g arr ':a:*'; echo ${(o)arr}; zstyle -g "
     "arr ':a:*' s; echo $arr",
     ":a:* :b:*\ns t\nv\n",
     "", 0},
    {"zstyle ':a:*' s v; zstyle ':a:*' t w; zstyle -d ':a:*' s; zstyle -L; "
     "zstyle -d; zstyle -L; echo $?",
     "zstyle ':a:*' t w\n0\n",
     "", 0},
    {"zstyle -x; echo $?; zstyle -sx a b c; echo $?; zstyle -s a; echo $?; "
     "zstyle -s a b c d e; echo $?",
     "1\n1\n1\n1\n",
     "jfsh: zstyle: invalid option: -x\n"
     "jfsh: zstyle: invalid argument: -sx\n"
     "jfsh: zstyle: not enough arguments\njfsh: zstyle: too many arguments\n", 0},
  };
  for (const auto& e : cases) check(e);
}

// zformat -f, -F and -a.
TEST(ModulesM10Review, ZformatAsBinZformat) {
  const Expect cases[] = {
    {"zformat -f out '[%5a][%-5a][%.2a][%5.2a] %b %%' a:hello b:2; echo "
     "\"$out\"",
     "[hello][hello][he][he   ] 2 %\n",
     "", 0},
    {"zformat -f out '%(a.yes.no) %(b.yes.no) %(c.yes.no) %(2a.ge2.lt2) "
     "%(-2a.x.y)' a:3 b:0 c:; echo \"$out\"",
     "no yes yes lt2 y\n",
     "", 0},
    {"zformat -F out '%(a.set.unset) %(b.set.unset) %(3a.long.short)' "
     "a:hello b:; echo \"$out\"",
     "set unset long\n",
     "", 0},
    {"zformat -a arr ' -- ' 'a:desc a' 'bbb:desc b' 'c\\:d:desc c' nocolon; "
     "print -l $arr",
     "a   -- desc a\nbbb -- desc b\nc:d -- desc c\nnocolon\n",
     "", 0},
    {"zformat -f out a -x:1; echo $?; zformat -x out a b; echo $?; zformat "
     "-f; echo $?",
     "1\n1\n1\n",
     "jfsh: zformat: invalid argument: -x:1\n"
     "jfsh: zformat: invalid option: -x\n"
     "jfsh: zformat: not enough arguments\n", 0},
  };
  for (const auto& e : cases) check(e);
}

// zparseopts' specs and flags.
TEST(ModulesM10Review, ZparseoptsAsBinZparseopts) {
  const Expect cases[] = {
    {"set -- -a1 -bb -c rest; zparseopts -D a:=A b+=B c=C; echo $A / $B / "
     "$C / $@",
     "-a 1 / -b -b / -c / rest\n",
     "", 0},
    {"set -- -a -b; zparseopts a::=A b=B; echo $A / $B; set -- -afoo; "
     "zparseopts a:-=D; echo $D",
     "-a / -b\n-afoo\n",
     "", 0},
    {"set -- -x -a; zparseopts -F a=A; echo $?; zparseopts -E -D a=A; echo "
     "$A / $@",
     "1\n-a / -x\n",
     "jfsh: zparseopts: bad option: -x\n", 0},
    {"set -- -a 1 -a 2; zparseopts -A H a+:; echo $H[-a]; set -- -a 1 -b 2; "
     "zparseopts -A I a: b:; echo ${(kv)I}",
     "12\n-a 1 -b 2\n",
     "", 0},
    {"set -- -long 1 -l; zparseopts long:=L l=S; echo $L / $S; set -- -b; "
     "zparseopts -M a=A b=a; echo $A",
     "-long 1 / -l\n-b\n",
     "", 0},
    {"zparseopts; echo $?; zparseopts a; echo $?; zparseopts a=A a=B; echo "
     "$?; zparseopts -a A -a B a; echo $?",
     "1\n1\n1\n1\n",
     "jfsh: zparseopts: not enough arguments\n"
     "jfsh: zparseopts: no default array defined: a\n"
     "jfsh: zparseopts: option defined more than once: a\n"
     "jfsh: zparseopts: default array given more than once\n", 0},
  };
  for (const auto& e : cases) check(e);
}

// zregexparse's regex of patterns.
TEST(ModulesM10Review, ZregexparseAsBinZregexparse) {
  const Expect cases[] = {
    {"zregexparse p q abc /a/ /b/ /c/; echo $? $p $q; zregexparse p q abd "
     "/a/ /b/ /c/; echo $? $p $q",
     "0 3 3\n1 2 2\n",
     "", 0},
    {"zregexparse p q abbbc /a/ /b/ '#' /c/; echo $? $p $q",
     "0 5 5\n",
     "", 0},
    {"zregexparse p q ab /a/ ':echo action-a' /b/ ':echo action-b'; echo $?",
     "0\n",
     "", 0},
    {"zregexparse p q abc /a/ '(' /b/; echo $?; zregexparse p q abc 'bad'; "
     "echo $?; zregexparse p q; echo $?",
     "3\n3\n1\n",
     "jfsh: zregexparse: not enough regex arguments\n"
     "jfsh: zregexparse: invalid regex : bad\n"
     "jfsh: zregexparse: not enough arguments\n", 0},
  };
  for (const auto& e : cases) check(e);
}

// strftime's checks and formats.
TEST(ModulesM10Review, StrftimeAsBinStrftime) {
  const Expect cases[] = {
    {"zmodload zsh/datetime; TZ=UTC strftime '%Y-%m-%d %H:%M:%S' 86400; "
     "TZ=UTC strftime -s v %Y 0; echo $v",
     "1970-01-02 00:00:00\n1970\n",
     "", 0},
    {"zmodload zsh/datetime; strftime %Y 1e3; strftime %Y 0 1000000000; "
     "strftime -s 1x %Y; strftime -r %Y",
     "",
     "jfsh: strftime: 1e3: invalid decimal number\n"
     "jfsh: strftime: 1000000000: invalid nanosecond value\n"
     "jfsh: strftime: not an identifier: 1x\n"
     "jfsh: strftime: timestring expected\n", 1},
    {"zmodload zsh/datetime; TZ=UTC strftime -r '%Y-%m-%d' 2020-01-02; "
     "TZ=UTC strftime -rq %Y 2020x; echo $?; strftime -r %Y abc; echo $?",
     "1577923200\n1577836800\n0\n1\n",
     "jfsh: strftime: format not matched\n", 0},
    {"zmodload zsh/datetime; TZ=UTC strftime '%N %3. %K %L %f' 0 123456789; "
     "TZ=UTC strftime -n %Y 0; echo",
     "123456789 123 0 12 1\n1970\n",
     "", 0},
    {"zmodload zsh/datetime; typeset -p EPOCHSECONDS EPOCHREALTIME "
     "epochtime; unset EPOCHSECONDS; echo after",
     "typeset -ir EPOCHSECONDS\ntypeset -Fr EPOCHREALTIME\n"
     "typeset -ar epochtime\n",
     "jfsh: read-only variable: EPOCHSECONDS\n", 1},
  };
  for (const auto& e : cases) check(e);
}

// zsh/mathfunc's functions.
TEST(ModulesM10Review, MathfuncAsZsh) {
  const Expect cases[] = {
    {"zmodload zsh/mathfunc; echo $(( abs(-3) )) $(( abs(-3.5) )) $(( "
     "int(-3.9) )) $(( floor(2.5) )) $(( fmod(7,3) )) $(( ilogb(8) ))",
     "3 3.5 -3 2. 1. 3\n",
     "", 0},
    {"zmodload zsh/mathfunc; echo $(( atan(1,1) )) $(( copysign(3,-1) )) "
     "$(( ldexp(1.5, 2.9) )) $(( jn(1, 0) ))",
     "0.78539816339744828 -3. 6. 0.\n",
     "", 0},
    {"zmodload zsh/mathfunc; seed=0123456789ab; a=$(( rand48(seed) )); echo "
     "$seed",
     "edf27e24d1e7\n",
     "", 0},
    {"zmodload zsh/mathfunc; echo $(( sqrt(1,2) ))",
     "",
     "jfsh: wrong number of arguments: sqrt(1,2)\n", 1},
    {"zmodload zsh/mathfunc; echo $(( atan2(1,1) ))",
     "",
     "jfsh: unknown function: atan2\n", 1},
    {"zmodload -F zsh/mathfunc f:sqrt; echo $(( sqrt(4) )); echo $(( cos(0) "
     "))",
     "2.\n",
     "jfsh: unknown function: cos\n", 1},
  };
  for (const auto& e : cases) check(e);
}

// zsh/system's builtins and parameters.
TEST(ModulesM10Review, SystemAsZsh) {
  const Expect cases[] = {
    {"zmodload zsh/system; syserror ENOENT; syserror -p 'x: ' 13; syserror "
     "-e v 2; echo $v; syserror EFOO; echo $?",
     "No such file or directory\n2\n",
     "No such file or directory\nx: Permission denied\n", 0},
    {"zmodload zsh/system; echo ${errnos[(i)ENOENT]} $errnos[13] "
     "${(t)errnos}; echo ${(ok)sysparams}",
     "2 EACCES array-readonly-hide-hideval-special\npid ppid procsubstpid\n",
     "", 0},
    {"(d=$(mktemp -d) && cd $d && trap \"cd / && rm -rf $d\" EXIT && "
     "zmodload zsh/system; printf abcdef > f; sysopen -r -u fd f; sysread "
     "-i $fd -s 3 x; echo $x; sysseek -u $fd -w end -2; sysread -i $fd -c n "
     "y; echo $? $y $n $(( systell(fd) )); exec {fd}<&-)",
     "abc\n0 ef 2 6\n",
     "", 0},
    {"(d=$(mktemp -d) && cd $d && trap \"cd / && rm -rf $d\" EXIT && "
     "zmodload zsh/system; sysopen -w -u fd -o creat,trunc out; syswrite -c "
     "n -o $fd data; exec {fd}>&-; cat out; echo \" $n\")",
     "data 4\n",
     "", 0},
    {"zmodload zsh/system; sysopen /dev/null; sysopen -u 1x /dev/null; "
     "sysopen -u fd -o foo /dev/null; sysread -i abc; syswrite -o -1 x; "
     "sysseek -w bad 0",
     "",
     "jfsh: sysopen: file descriptor not specified\n"
     "jfsh: sysopen: not an identifier: 1x\n"
     "jfsh: sysopen: unsupported option: foo\n\n"
     "jfsh: sysread: integer expected: abc\n"
     "jfsh: syswrite: integer expected: -1\n"
     "jfsh: sysseek: unknown argument to -w: bad\n", 1},
    {"zmodload zsh/system; zsystem; zsystem foo; zsystem supports flock; "
     "echo $?; zsystem supports x; echo $?; zsystem flock; zsystem flock -u "
     "99; echo $?",
     "0\n1\n1\n",
     "jfsh: zsystem: not enough arguments\n"
     "jfsh: zsystem: unknown subcommand: foo\n"
     "jfsh: zsystem: flock: not enough arguments\n"
     "jfsh: zsystem: flock: file descriptor 99 not in use for locking\n", 0},
  };
  for (const auto& e : cases) check(e);
}

// zsh/files' builtins.
TEST(ModulesM10Review, FilesAsZsh) {
  const Expect cases[] = {
    {"(d=$(mktemp -d) && cd $d && trap \"cd / && rm -rf $d\" EXIT && "
     "zmodload zsh/files; mkdir -p d/e/f; mkdir d; mkdir -m 8 x; ls -d "
     "d/e/f)",
     "d/e/f\n",
     "jfsh: mkdir: cannot make directory `d': file exists\n"
     "jfsh: mkdir: invalid mode `8'\n", 0},
    {"(d=$(mktemp -d) && cd $d && trap \"cd / && rm -rf $d\" EXIT && "
     "zmodload zsh/files; mkdir -p d/e; : > d/e/f; rm d; echo $?; rm -r d; "
     "echo $?; ls; rm -rf nope; echo $?)",
     "1\n0\n0\n",
     "jfsh: rm: d: is a directory\n", 0},
    {"(d=$(mktemp -d) && cd $d && trap \"cd / && rm -rf $d\" EXIT && "
     "zmodload zsh/files; : > a; mkdir d; ln a b; ln a b; ln -s a l; mv a "
     "d; ls d; mv b c d2; echo $?)",
     "a\n1\n",
     "jfsh: ln: `a': file exists\n"
     "jfsh: mv: last of many arguments must be a directory\n", 0},
    {"(d=$(mktemp -d) && cd $d && trap \"cd / && rm -rf $d\" EXIT && "
     "zmodload zsh/files; : > f; chmod 600 f; ls -l f | cut -c1-10; chmod "
     "u+x f; chown nosuchuserxyz f; chgrp nosuchgroupxyz f)",
     "-rw-------\n",
     "jfsh: chmod: invalid mode `u+x'\n"
     "jfsh: chown: nosuchuserxyz: no such user\n"
     "jfsh: chgrp: nosuchgroupxyz: no such group\n", 1},
  };
  for (const auto& e : cases) check(e);
}

// zstat's options and elements.
TEST(ModulesM10Review, StatAsBinStat) {
  const Expect cases[] = {
    {"(d=$(mktemp -d) && cd $d && trap \"cd / && rm -rf $d\" EXIT && "
     "zmodload zsh/stat; printf abc > f; : > g; zstat +size f g; zstat -A a "
     "+size f g; echo $a; zstat -H h f; echo $h[size] ${#h})",
     "f 3\ng 0\n3 0\n3 14\n",
     "", 0},
    {"(d=$(mktemp -d) && cd $d && trap \"cd / && rm -rf $d\" EXIT && "
     "zmodload zsh/stat; : > f; chmod 644 f; zstat +mode f; zstat -s +mode "
     "f; zstat -o +mode f; zstat -rs +mode f; ln -s f l; zstat +link l; "
     "zstat -L -s +mode l)",
     "33188\n-rw-r--r--\n0100644\n33188 (-rw-r--r--)\nf\nlrwxr-xr-x\n",
     "", 0},
    {"(d=$(mktemp -d) && cd $d && trap \"cd / && rm -rf $d\" EXIT && "
     "zmodload zsh/stat; : > f; touch -t 202001020304.05 f; TZ=UTC zstat -g "
     "+mtime f; TZ=UTC zstat -F '%Y-%m-%d' -gr +mtime f)",
     "Thu Jan  2  8:04:05 UTC 2020\n1577952245 (2020-01-02)\n",
     "", 0},
    {"zmodload zsh/stat; zstat -l; zstat +nope /; zstat; zstat -A a -H h /; "
     "zstat /no/such; zstat -x /",
     "device inode mode nlink uid gid rdev size atime mtime ctime blksize "
     "blocks link\n",
     "jfsh: zstat: nope: no such stat element\njfsh: zstat: no files given\n"
     "jfsh: zstat: both array and hash requested\n"
     "jfsh: zstat: /no/such: no such file or directory\n"
     "jfsh: zstat: bad option: -x\n", 1},
  };
  for (const auto& e : cases) check(e);
}

// =~ runs zsh/regex's condition, or zsh/pcre's under RE_MATCH_PCRE.
TEST(ModulesM10Review, RegexAndPcreConditions) {
  const Expect cases[] = {
    {"[[ foobar =~ o+(b)a ]]; echo $? $MATCH $match $MBEGIN $MEND $mbegin "
     "$mend; zmodload",
     "0 ooba b 2 5 4 4\nzsh/main\nzsh/regex\n",
     "", 0},
    {"setopt bashrematch; MATCH=keep; [[ abc =~ b(c) ]]; echo $? "
     "$BASH_REMATCH $MATCH",
     "0 bc c keep\n",
     "", 0},
    {"[[ ABC =~ b ]]; echo $?; setopt nocasematch; [[ ABC =~ b ]]; echo $?",
     "1\n0\n",
     "", 0},
    {"setopt rematchpcre; [[ abc =~ 'b(c)' ]]; echo $? $MATCH $match; "
     "zmodload",
     "0 bc c\nzsh/main\nzsh/pcre\n",
     "", 0},
    {"zmodload zsh/pcre; pcre_compile 'b(c)'; pcre_match -v m -a a -b abcd; "
     "echo $? $m $a $ZPCRE_OP; pcre_match -n 3 abc; echo $?",
     "0 bc c 1 3\n1\n",
     "", 0},
    {"zmodload -F zsh/regex -C:regex-match; [[ a =~ a ]]; echo $?",
     "0\n",
     "", 0},
  };
  for (const auto& e : cases) check(e);
}

// zsh/example, the module with every kind of feature.
TEST(ModulesM10Review, ExampleAsZsh) {
  const Expect cases[] = {
    {"zmodload zsh/example; example -fl a b; echo $exint $exstr $exarr; "
     "zmodload -u zsh/example",
     "The example module has now been set up.\nOptions: fl\nArguments: a b\n"
     "Name: example\n\nInteger Parameter: 42\nString Parameter: example\n"
     "Array Parameter: example array\n2 a a b\n"
     "Thank you for using the example module.  Have a nice day.\n",
     "", 0},
    {"zmodload zsh/example; [[ -len abc 3 ]] && echo y; [[ ex -ex ample ]] "
     "&& echo x; echo $(( sum(1,2,3.5) )) $(( length( ab ) ))",
     "The example module has now been set up.\ny\nx\n6.5 4\n",
     "", 0},
    {"zmodload -F zsh/example b:example; example | tail -3",
     "The example module has now been set up.\nInteger Parameter: 42\n"
     "String Parameter: example\nArray Parameter: example array\n",
     "", 0},
    {"zmodload zsh/example; exint=3+4; echo $exint; exarr=x; echo $exarr "
     "${(t)exarr}; exint=(1 2); echo $exint; exstr=(a b); echo $exstr "
     "${(t)exstr}",
     "The example module has now been set up.\n7\nx scalar\n1 2\na b array\n",
     "", 0},
    {"zmodload -ap zsh/example exint; exint+=2; echo $exint; exint+=2; echo "
     "$exint",
     "The example module has now been set up.\n44\n46\n",
     "", 0},
    {"zmodload -ap zsh/example exarr; exarr+=x; echo $exarr ${(t)exarr}",
     "The example module has now been set up.\nexample array x array\n",
     "", 0},
    {"zmodload zsh/example; typeset -A exarr; exarr[a]=1; typeset -p exarr",
     "The example module has now been set up.\ntypeset -A exarr=( [a]=1 )\n",
     "", 0},
    {"zmodload zsh/example; typeset -A exstr; exstr=(a 1); typeset -p "
     "exstr; typeset -A exint; exint[k]=v; typeset -p exint",
     "The example module has now been set up.\ntypeset -A exstr=( [a]=1 )\n"
     "typeset -A exint=( [k]=v )\n",
     "", 0},
    {"zmodload zsh/example; typeset -a exstr; exstr+=(x); typeset -p exstr; "
     "typeset -i exarr; exarr=3+4; typeset -p exarr",
     "The example module has now been set up.\ntypeset -a exstr=( x )\n"
     "typeset -i exarr=7\n",
     "", 0},
  };
  for (const auto& e : cases) check(e);
}

}  // namespace jfsh::test
