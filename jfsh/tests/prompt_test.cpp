// Prompt expansion (M9): the expected strings are what zsh 5.9 prints for
// `print -rP` of the same prompt (TERM=xterm-256color), except where a
// comment says otherwise.

#include <unistd.h>

#include <gtest/gtest.h>
#include <shell_fixture.h>

#include <jfsh/core/aliases.h>
#include <jfsh/prompt/prompt.h>

namespace jfsh {
namespace {

using test::Shell;

str P(Shell& sh, strv prompt) {
  PromptRenderer r(sh.params, sh.opts);
  r.set_hooks(sh.exec.prompt_hooks());
  return r.render(prompt);
}

TEST(Prompt, AttributesAndColours) {
  Shell sh;
  EXPECT_EQ(P(sh, "%B%b|%U%u|%S%s"), "\x1b[1m\x1b[0m|\x1b[4m\x1b[24m|\x1b[7m\x1b[27m");
  EXPECT_EQ(P(sh, "%F{red}x%f|%F{9}x%f|%F{123}x%f|%K{blue}x%k"),
            "\x1b[31mx\x1b[39m|\x1b[38;5;9mx\x1b[39m|\x1b[38;5;123mx\x1b[39m|\x1b[44mx\x1b[49m");
  EXPECT_EQ(P(sh, "%F{#ff0000}x%f|%1F|%E"), "\x1b[38;2;255;0;0mx\x1b[39m|\x1b[31m|\x1b[K");
  // Bold brings the colours back; %b resets everything, then restores
  // what is still on.
  EXPECT_EQ(P(sh, "%F{red}a%Bb%bc%f"), "\x1b[31ma\x1b[1m\x1b[31mb\x1b[0m\x1b[31mc\x1b[39m");
  EXPECT_EQ(P(sh, "%B%F{red}%K{blue}%U%S%bX"),
            "\x1b[1m\x1b[31m\x1b[44m\x1b[4m\x1b[7m\x1b[0m\x1b[7m\x1b[4m\x1b[31m\x1b[44mX");
  EXPECT_EQ(P(sh, "%F{default}a%F{bogus}b%F{-1}c%F{256}d%K{0}e"),
            "\x1b[39ma\x1b[39mb\x1b[39mc\x1b[39md\x1b[40me");
}

TEST(Prompt, B35UnknownEscapesExpandToNothing) {
  Shell sh;
  EXPECT_EQ(P(sh, "%Bx%b"), "\x1b[1mx\x1b[0m");
  EXPECT_EQ(P(sh, "100%%|%)|%z|%5%|"), "100%|)||%|");
}

TEST(Prompt, B34DirectoriesWithNumericArguments) {
  Shell sh;
  sh.params.set_scalar("HOME", "/Users/nobody");
  sh.params.set_scalar("PWD", "/usr/local/bin");
  EXPECT_EQ(P(sh, "%1~|%2~|%-1~|%2d|%-2/|%c|%C|%.|%3c"),
            "bin|local/bin|/usr|local/bin|/usr/local|bin|bin|bin|/usr/local/bin");
  sh.params.set_scalar("HOME", "/usr");
  EXPECT_EQ(P(sh, "%~|%1~|%2~|%3~"), "~/local/bin|bin|local/bin|~/local/bin");
  sh.params.set_scalar("PWD", "/usr");
  EXPECT_EQ(P(sh, "%~|%1~|%-1~|%/"), "~|~|~|/usr");
  sh.params.set_scalar("PWD", "/");
  EXPECT_EQ(P(sh, "%~|%1~|%-1~|%/|%c"), "/|/|/|/|/");
}

TEST(Prompt, B36TildeOnlyAtAComponentBoundary) {
  Shell sh;
  sh.params.set_scalar("HOME", "/Users/a");
  sh.params.set_scalar("PWD", "/Users/ab");
  EXPECT_EQ(P(sh, "%~"), "/Users/ab");
  sh.params.set_scalar("PWD", "/Users/a/x");
  EXPECT_EQ(P(sh, "%~"), "~/x");
}

TEST(Prompt, Conditionals) {
  Shell sh;
  sh.run("true");
  EXPECT_EQ(P(sh, "%(?.ok.bad)|%(1?.one.not)|%(#.r.u)"), str("ok|not|") + (::geteuid() == 0 ? "r" : "u"));
  sh.run("false");
  EXPECT_EQ(P(sh, "%?|%(?.ok.bad)|%(1?.one.not)"), "1|bad|one");
  // Any delimiter; nested; a ) in the true branch is text.
  EXPECT_EQ(P(sh, "%(x.a.b)|%(?,a,b)|%(1?.%(?.r.u).no)|%(1?.(a).b)"), "|b|u|(a)");
  sh.run("psvar=(a b)");
  EXPECT_EQ(P(sh, "%v|%2v|%3v|%(2v.has2.no2)|%(3v.has3.no3)"), "a|b||has2|no3");
  EXPECT_EQ(P(sh, "%(1j.jobs.nojobs)|%j"), "nojobs|0");
  // Not compared with zsh, whose print -P counts the line differently.
  EXPECT_EQ(P(sh, "%(5l.long.short)|abcdef%(5l.long.short)"), "short|abcdeflong");
  // The branch not taken leaves its colours behind.
  EXPECT_EQ(P(sh, "%(?.x.%F{red})y"), "xy");
}

TEST(Prompt, Truncation) {
  Shell sh;
  sh.params.set_scalar("HOME", "/Users/nobody");
  sh.params.set_scalar("PWD", "/usr/local/bin");
  EXPECT_EQ(P(sh, "%5<..<abcdefghij|%5>..>abcdefghij|%5<<abcdefghij%<<xyz|%10<…<%~"),
            "..ij|abc..fghijxyz|…local/bin");
  EXPECT_EQ(P(sh, "%3>>abcdef%>>|end"), "abc|end");
  // Zero-width parts survive the cut and don't count.
  EXPECT_EQ(P(sh, "%4>.>%F{red}abcdef%f"), "\x1b[31mabc.\x1b[39m");
}

TEST(Prompt, LiteralEscapesAreZeroWidth) {
  Shell sh;
  PromptRenderer r(sh.params, sh.opts);
  str e = r.expand("%{\x1b[5m%}ab%F{red}c%f");
  EXPECT_EQ(PromptRenderer::visual_width(e), 3);
  EXPECT_EQ(PromptRenderer::strip_markers(e), "\x1b[5mab\x1b[31mc\x1b[39m");
  EXPECT_EQ(PromptRenderer::strip_markers(e, true), "abc");
  // Width counts characters, wide ones twice; only the last line counts.
  EXPECT_EQ(PromptRenderer::visual_width(r.expand("日本 é")), 6);
  EXPECT_EQ(PromptRenderer::visual_width(r.expand("first line\n%% ")), 2);
}

TEST(Prompt, ValuesAndDates) {
  Shell sh;
  sh.params.set_scalar("SHLVL", "3");
  EXPECT_EQ(P(sh, "%L|%(3L.deep.shallow)|%(4L.deep.shallow)"), "3|deep|shallow");
  EXPECT_EQ(P(sh, "%N:%i"), "jfsh:0");
  EXPECT_EQ(P(sh, "%D{%Y}").size(), 4u);
  EXPECT_EQ(P(sh, "%D{}"), "");
  str t = P(sh, "%*");
  EXPECT_EQ(t.size() - t.find(':'), 6u);   // H:MM:SS, hours not padded
}

TEST(Prompt, PromptSubstAndBang) {
  Shell sh;
  sh.run("setopt promptsubst; x=val");
  // Quotes stay; a backslash before $ goes.
  EXPECT_EQ(P(sh, "a 'q' \"d\" $x $(echo cmd) $((1+2)) \\$x %%"),
            "a 'q' \"d\" val cmd 3 $x %");
  sh.run("unsetopt promptsubst");
  EXPECT_EQ(P(sh, "$x"), "$x");
  sh.run("setopt promptbang");
  EXPECT_EQ(P(sh, "a!b!!c"), "a0b!c");
  sh.run("unsetopt promptpercent");
  EXPECT_EQ(P(sh, "%~!!"), "%~!");
}

TEST(Prompt, PrintDashPAndXtrace) {
  Shell sh;
  EXPECT_EQ(sh.run("print -P '%B%%%b'").out, "\x1b[1m%\x1b[0m\n");
  EXPECT_EQ(sh.run("PS4='%N:%i> '; set -x; echo a; set +x").err, "jfsh:1> echo a\njfsh:1> set +x\n");
}

TEST(Prompt, DefaultsFollowZsh) {
  Shell sh;
  PromptRenderer r(sh.params, sh.opts);
  // A shell that isn't interactive starts with empty PS1 and PS2 (zsh's
  // setupvals); PS3 and PS4 are set all the same.
  EXPECT_EQ(PromptRenderer::strip_markers(r.continuation()), "");
  EXPECT_EQ(PromptRenderer::strip_markers(r.select()), "?# ");
  EXPECT_EQ(sh.run("print -r -- \"[$PS1][$PS2][$PS3][$PS4]\"").out, "[][][?# ][+%N:%i> ]\n");
  // Unset, a prompt is empty; PROMPT unset empties PS1 and is unset itself.
  sh.run("PS2='>> '; unset PS2");
  r.reload();
  EXPECT_EQ(PromptRenderer::strip_markers(r.continuation()), "");
  EXPECT_EQ(sh.run("PS1=q; unset PROMPT; print -r -- \"[${PS1-u}][${PROMPT-u}]\"").out, "[][u]\n");
  EXPECT_EQ(sh.run("PROMPT=z; print -r -- \"[${PS1-u}][${PROMPT-u}]\"").out, "[z][z]\n");
  // One string, a flag each: PROMPT doesn't set an unset PS1, but the
  // prompt shown is the string (zsh's zle reads it whatever the flags).
  EXPECT_EQ(sh.run("unset PS1; PROMPT=x; print -r -- \"[${PS1-u}][$PROMPT]\"").out, "[u][x]\n");
  r.reload();
  EXPECT_EQ(PromptRenderer::strip_markers(r.primary()), "x");
  sh.run("unset PS1; prompt='R> '");
  r.reload();
  EXPECT_EQ(PromptRenderer::strip_markers(r.primary()), "R> ");
  // RPROMPT and RPS1 are one string too, both unset at the start.
  EXPECT_EQ(sh.run("print ${+RPS1} ${+RPROMPT}; RPROMPT=rr; RPS1=ss; print $RPROMPT").out,
            "0 0\nss\n");
  r.reload();
  EXPECT_EQ(PromptRenderer::strip_markers(r.right()), "ss");
  // An exported PS1 keeps its environment entry when PROMPT is assigned.
  EXPECT_EQ(sh.run("export PS1=a; PROMPT=b; env | grep '^PS1='; print $PS1").out,
            "PS1=a\nb\n");
  EXPECT_EQ(sh.run("PS1=c; env | grep '^PS1='").out, "PS1=c\n");
  // A special scalar refuses an array, its value kept.
  auto arr = sh.run("histchars=(a b); print -r -- $histchars");
  EXPECT_EQ(arr.err, "jfsh: histchars: attempt to assign array value to non-array\n");
}

// ---- M9 review: zsh 5.9's putpromptchar and prompttrunc ----------------------

TEST(PromptM9Review, AttributesAlwaysGoOut) {
  Shell sh;
  // %b, %s and %u are sent even when nothing was on, and the attributes
  // still on come back after them (tsetcap's TSC_DIRTY).
  EXPECT_EQ(P(sh, "a%bb"), "a\x1b[0mb");
  EXPECT_EQ(P(sh, "%B%Ba%b%b"), "\x1b[1m\x1b[1ma\x1b[0m\x1b[0m");
  EXPECT_EQ(P(sh, "%Sa%Ub%sc%u"), "\x1b[7ma\x1b[4mb\x1b[27m\x1b[4mc\x1b[24m");
  // %F without braces takes the number before it: none is 0, black.
  EXPECT_EQ(P(sh, "%Fred"), "\x1b[30mred");
  EXPECT_EQ(P(sh, "%2Fa%f"), "\x1b[32ma\x1b[39m");
}

TEST(PromptM9Review, ConditionalsAndTheirTests) {
  Shell sh;
  sh.run("cd /usr/share/man; psvar=(a b c)");
  EXPECT_EQ(P(sh, "%(u.T.F)"), "");        // not a test: neither branch
  EXPECT_EQ(P(sh, "%-(?.T.F)"), "F");      // %-( is %1(
  EXPECT_EQ(P(sh, "%(?.T)"), "T)");        // the string ends in the branch
  EXPECT_EQ(P(sh, "%(?.a%)b.c)"), "a)b");
  EXPECT_EQ(P(sh, "%(3/.yes.no)"), "yes");
  EXPECT_EQ(P(sh, "%(0#.r.u)"), ::geteuid() == 0 ? "r" : "u");
  EXPECT_EQ(P(sh, "%v %-1v %(2v.T.F)"), "a c T");
  // psvar given a scalar is an array of one.
  sh.run("psvar=x");
  EXPECT_EQ(P(sh, "%v"), "x");
}

TEST(PromptM9Review, TruncationAsPrompttrunc) {
  Shell sh;
  EXPECT_EQ(P(sh, "%5[..]abcdefghij"), "abcd.");
  EXPECT_EQ(P(sh, "%3<.<%Babcdef%b"), ".\x1b[1mef\x1b[0m");
  EXPECT_EQ(P(sh, "%10<...<%B%F{red}abcdefghijklmnop%f%b|"),
            "...\x1b[1m\x1b[31mklmnop\x1b[39m\x1b[0m|");
  // At the right the last character kept may go past the width.
  EXPECT_EQ(P(sh, "%3>>日本語日本語"), "日本");
  // %-N< is N columns short of $COLUMNS.
  sh.run("COLUMNS=20");
  EXPECT_EQ(P(sh, "%-5<..<abcdefghijklmnopqrstuvwxyz"), "..nopqrstuvwxyz");
  EXPECT_EQ(P(sh, "x%"), "x");
  EXPECT_EQ(P(sh, "%"), "");
}

TEST(PromptM9Review, TimeAndDepth) {
  Shell sh;
  EXPECT_EQ(P(sh, "%D{%%}"), "%");
  EXPECT_EQ(P(sh, "%D{%3.}").size(), 3u);
  EXPECT_EQ(P(sh, "%D{%6.}").size(), 6u);
  // %t is strftime's %l:%M%p, the hour padded with a blank.
  EXPECT_EQ(P(sh, "%t").size(), 7u);
  EXPECT_EQ(sh.run("PS4='[%e]'; f() { echo y }; set -x; f; set +x").err,
            "[0]f\n[1]echo y\n[0]set +x\n");
}

TEST(PromptM9Review, PromptParametersAreOneAnother) {
  Shell sh;
  EXPECT_EQ(sh.run("PROMPT='abc'; print -r -- $PS1; PS2=x; print -r -- $PROMPT2; "
                   "print -r -- $PS3 $PROMPT4").out,
            "abc\nx\n?#  +%N:%i> \n");
}

// ---- M9 review, round 1 ------------------------------------------------------
//
// Expected values from zsh -f (scratchpad m9rv1/p/r9.py, r14.py).

// ztrftime's E, O and the other flags after a plain start go to strftime
// with the next character (goto morefmt); at the end, a lone `%`.
TEST(PromptM9Review, TimeFlagsGoToStrftime) {
  Shell sh;
  EXPECT_EQ(sh.run("print -rP -- '%D{%Q|%E|%-E|%5}'").out, "Q|||%\n");
}

// The names sharing a prompt string keep their own environment entries
// (each changes only when that name is assigned) and their own unset flag
// (unsetting a name already unset leaves the string alone).  Expected
// values from zsh -f (scratchpad m9rv4r/p/t_par.py).
TEST(PromptM9Review, SharedStringsAndTheEnvironment) {
  Shell sh;
  EXPECT_EQ(sh.run("export PROMPT=a; PS1=b; env | grep '^PROMPT='").out, "PROMPT=a\n");
  EXPECT_EQ(sh.run("export RPROMPT=r; RPS1=s; env | grep '^RPROMPT='").out, "RPROMPT=r\n");
  EXPECT_EQ(sh.run("export RPROMPT2=r; RPS2=s; env | grep '^RPROMPT2='").out, "RPROMPT2=r\n");
  Shell u;
  EXPECT_EQ(u.run("export PS1=a; export PROMPT=b; unset PS1; env | grep -E '^(PROMPT|PS1)='").out,
            "PROMPT=b\n");
  Shell x;
  EXPECT_EQ(x.run("export PS1=a PROMPT=b; env | grep -E '^(PROMPT|PS1)=' | sort").out,
            "PROMPT=b\nPS1=a\n");
  Shell y;
  EXPECT_EQ(
      y.run("export PS1=a; export PROMPT=b; unset PROMPT; env | grep -E '^(PROMPT|PS1)='").out,
      "PS1=a\n");
  Shell r;
  EXPECT_EQ(r.run("RPROMPT=r; unset RPS1; print -r -- \"[$RPROMPT]\"").out, "[r]\n");
  EXPECT_EQ(r.run("RPROMPT2=a; unset RPS2; print -r -- \"[$RPROMPT2]\"").out, "[a]\n");
  EXPECT_EQ(r.run("unset PS1; PROMPT='Q> '; unset PS1; print -r -- \"[$PROMPT]\"").out, "[Q> ]\n");
  Shell e;
  EXPECT_EQ(e.run("RPROMPT=r; export RPS1; env | grep -c '^RPS1='; print ${+RPS1}").out, "0\n0\n");
  auto p = e.run("typeset -p RPS1; print $?");
  EXPECT_EQ(p.out, "0\n");
  EXPECT_EQ(p.err, "");
}

// A special scalar refuses an array however it comes, with zsh's
// messages (setaparam, typeset_single).
TEST(PromptM9Review, SpecialScalarsRefuseArrays) {
  Shell sh;
  EXPECT_EQ(sh.run("set -A histchars a b").err,
            "jfsh: histchars: attempt to assign array value to non-array\n");
  EXPECT_EQ(sh.run("read -A histchars <<< 'a b'").err,
            "jfsh: histchars: attempt to assign array value to non-array\n");
  EXPECT_EQ(sh.run("typeset histchars=(a)").err,
            "jfsh: typeset: histchars: can't assign array value to non-array special\n");
  EXPECT_EQ(sh.run("f() { local histchars=(a); }; f").err,
            "jfsh: local: histchars: can't change type of a special parameter\n");
  EXPECT_EQ(sh.run("typeset -a histchars").err,
            "jfsh: typeset: histchars: can't change type of a special parameter\n");
  EXPECT_EQ(sh.run("typeset -A PS1").err,
            "jfsh: typeset: PS1: can't change type of a special parameter\n");
  for (const char* name : {"PS1", "PS2", "PS3", "PS4", "SPROMPT", "PROMPT4"})
    EXPECT_EQ(sh.run(str(name) + "=(a)").err,
              "jfsh: " + str(name) + ": attempt to assign array value to non-array\n");
  // Nor does a string special become a number; an integer one may.
  for (const char* t : {"-i PS1", "-i PS1=3", "-F PS2", "-E SPROMPT", "-i PROMPT",
                        "-i histchars"}) {
    str name(strv(t).substr(3, strv(t).find('=', 3) - 3));
    EXPECT_EQ(sh.run(str("typeset ") + t + "; print x").err,
              "jfsh: typeset: " + name + ": can't change type of a special parameter\n");
  }
  EXPECT_EQ(sh.run("f() { local -i PS4=2; }; f").err,
            "jfsh: local: PS4: can't change type of a special parameter\n");
  EXPECT_EQ(sh.run("typeset -F SECONDS").err, "");
  // setaparam's failure ends the line, but bin_set returns 0, and
  // bin_read its own status.
  EXPECT_EQ(sh.run("false; set -A PS3 a b; print no").rc, 0);
  EXPECT_EQ(sh.run("false; set -A PS3 a b; print no").out, "");
  auto ra = sh.run("false; read -A PS1 <<< 'a b'; print no");
  EXPECT_EQ(ra.rc, 0);
  EXPECT_EQ(ra.out, "");
  EXPECT_EQ(ra.err, "jfsh: PS1: attempt to assign array value to non-array\n");
  EXPECT_EQ(sh.run("printf x | read -A PS1; print no").rc, 1);
  // As setarrvalue: read-only comes first.
  for (const char* name : {"PS4", "PS1", "RPS1", "PROMPT", "histchars"}) {
    Shell r;
    EXPECT_EQ(r.run(str("readonly ") + name + "; " + name + "=(a)").err,
              str("jfsh: read-only variable: ") + name + "\n");
  }
  for (const char* t : {"LINENO=(a)", "PPID=(a)", "readonly PS4; set -A PS4 a",
                        "readonly PS4; read -A PS4 <<< a", "readonly PS4; PS4+=(a)"}) {
    Shell r;
    str name = strv(t).starts_with("readonly") ? "PS4" : str(strv(t).substr(0, strv(t).find('=')));
    EXPECT_EQ(r.run(t).err, "jfsh: read-only variable: " + name + "\n") << t;
  }
  // A subscript on a special scalar picks characters, unset or not: of
  // the empty string, or of what its getter gives.
  auto show = [](const char* names) {
    return str("print -r -- \"[") + names + "]\"";
  };
  for (const char* name : {"RPS1", "RPROMPT", "RPS2", "RPROMPT2"}) {
    Shell r;
    auto res = r.run(str(name) + "[1]=z; print -r -- \"[$" + name + "]\"");
    EXPECT_EQ(res.err, "") << name;
    EXPECT_EQ(res.out, "[z]\n") << name;
  }
  {
    Shell r;
    EXPECT_EQ(r.run("RPS1[1]+=w; RPS1[5]=q; " + show("${RPS1-U}|${RPROMPT-U}")).out, "[wq|U]\n");
    EXPECT_EQ(r.run("unset PS1; PS1[2,3]=yz; " + show("$PS1|$PROMPT")).out, "[yz|yz]\n");
    EXPECT_EQ(r.run("unset PROMPT; PROMPT[1]=z; " + show("$PS1|$PROMPT")).out, "[z|z]\n");
    EXPECT_EQ(r.run("RPS1=abc; unset RPROMPT; RPROMPT[2]=X; " + show("$RPS1|$RPROMPT")).out,
              "[aXc|aXc]\n");
    EXPECT_EQ(r.run("unset histchars; histchars[1]=z; " + show("$histchars|$HISTCHARS")).out,
              "[z^#|z^#]\n");
    EXPECT_EQ(r.run("unset histchars; histchars[2,3]=yz; " + show("$histchars")).out, "[!yz]\n");
    EXPECT_EQ(r.run("RPS1[1]=(a)").err,
              "jfsh: RPS1: attempt to assign array value to non-array\n");
  }
  EXPECT_EQ(sh.run("print -r -- $histchars").out, "!^#\n");
}

// A local of one name of a shared string puts back the string's value
// (copyparam's getfn), leaving the outer name unset as it was.
TEST(PromptM9Review, LocalOfASharedName) {
  auto run = [](const str& cmd, const char* a, const char* b) {
    Shell sh;
    return sh.run(cmd + "; print -r -- \"[${" + a + "-U}|${" + b + "-U}]\"").out;
  };
  for (auto [a, b] : {s::pair{"RPS1", "RPROMPT"}, s::pair{"RPS2", "RPROMPT2"},
                      s::pair{"PS1", "PROMPT"}, s::pair{"PS1", "prompt"},
                      s::pair{"PS2", "PROMPT2"}, s::pair{"PS3", "PROMPT3"},
                      s::pair{"PS4", "PROMPT4"}}) {
    str A = a, B = b;
    bool unset_b = A.starts_with("RPS");   // RPROMPT, RPROMPT2 start unset
    str bv = unset_b ? "U" : "a";
    EXPECT_EQ(run(A + "=a; f(){ local " + B + "=x }; f", a, b), "[a|" + bv + "]\n") << b;
    EXPECT_EQ(run(A + "=a; f(){ local " + B + " }; f", a, b), "[a|" + bv + "]\n") << b;
    EXPECT_EQ(run(A + "=a; f(){ typeset " + B + "=x }; f", a, b), "[a|" + bv + "]\n") << b;
    EXPECT_EQ(run(A + "=a; f(){ local " + B + "; " + A + "=m }; f", a, b), "[a|" + bv + "]\n")
        << b;
    // (Unsetting a PS name empties the string; an RPS one keeps it.)
    EXPECT_EQ(run(A + "=a; unset " + B + "; f(){ local " + B + "=x }; f", a, b),
              str(unset_b ? "[a|U]\n" : "[|U]\n")) << b;
    EXPECT_EQ(run(A + "=a; g(){ local " + B + "=y }; f(){ local " + B + "=x; g; "
                  "print -r -- \"in[$" + A + "]\" }; f", a, b),
              "in[x]\n[a|" + bv + "]\n") << b;
  }
  Shell sh;
  EXPECT_EQ(sh.run("unset HISTCHARS; histchars=@^#; f(){ local HISTCHARS=%^# }; f; "
                   "print -r -- \"[$histchars|${HISTCHARS-U}]\"").out,
            "[@^#|U]\n");
}

// `+=` through a name of a shared string reads the string, whether that
// name is set or not (IPDEF7's one pointer).
TEST(PromptM9Review, AppendThroughASharedName) {
  auto run = [](const char* cmd) {
    Shell sh;
    return sh.run(str(cmd) + "; print -r -- \"[${RPS1-U}|${RPROMPT-U}|${PS1-U}|${PROMPT-U}]\"").out;
  };
  EXPECT_EQ(run("RPS1=x; RPROMPT+=y"), "[xy|xy||]\n");
  EXPECT_EQ(run("RPROMPT=x; RPS1+=y"), "[xy|xy||]\n");
  EXPECT_EQ(run("unset PROMPT; PS1=x; PROMPT+=y"), "[U|U|xy|xy]\n");
  EXPECT_EQ(run("unset PS1; PROMPT=x; PS1+=y"), "[U|U|xy|xy]\n");
  EXPECT_EQ(run("RPS1=a; unset RPS1; RPROMPT+=b"), "[U|b||]\n");
  Shell sh;
  EXPECT_EQ(sh.run("RPS2=x; RPROMPT2+=y; print -r -- \"[$RPS2|$RPROMPT2]\"").out, "[xy|xy]\n");
  EXPECT_EQ(sh.run("f() { local RPS1=l; RPROMPT+=m; print -r -- \"in[$RPS1]\"; }; f").out,
            "in[lm]\n");
  EXPECT_EQ(sh.run("unset HISTCHARS; histchars=@^; HISTCHARS+=%; "
                   "print -r -- \"[$histchars|$HISTCHARS]\"").out,
            "[@^%|@^%]\n");
  EXPECT_EQ(sh.run("RPS1=x; RPROMPT+=(y)").err,
            "jfsh: RPROMPT: attempt to assign array value to non-array\n");
}

// zstrtol's warning when a prompt number has more digits than 64 bits.
TEST(PromptM9Review, NumbersTruncatedWithAWarning) {
  Shell sh;
  auto r = sh.run("print -P 'x%99999999999999999999>>abc'");
  EXPECT_EQ(r.out, "xabc\n");
  EXPECT_EQ(r.err, "jfsh: number truncated after 19 digits: 99999999999999999999>>abc\n");
  r = sh.run("print -P '%3(?.a.b)%99999999999999999999(?.a.b)'");
  EXPECT_EQ(r.out, "bb\n");
  EXPECT_EQ(r.err, "jfsh: number truncated after 19 digits: 99999999999999999999(?.a.b)\n");
}

}  // namespace
}  // namespace jfsh
