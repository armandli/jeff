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
  EXPECT_EQ(PromptRenderer::strip_markers(r.continuation()), "> ");
  EXPECT_EQ(PromptRenderer::strip_markers(r.select()), "?# ");
}

}  // namespace
}  // namespace jfsh
