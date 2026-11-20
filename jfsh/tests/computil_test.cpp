// M15 phase 15b: match specifications (compadd -M), the completion
// condition codes (-prefix, -suffix, -after, -between) and zsh/computil
// (comptags, comptry, compdescribe, compvalues, compfiles, compgroups).
// The editor runs in process on a pty (editor_rig.h); expected values are
// zsh 5.9's (`zsh -f`), taken by driving an interactive zsh the same way
// with the same completion functions.

#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include <editor_rig.h>
#include <gtest/gtest.h>
#include <shell_fixture.h>

#include <jfsh/core/aliases.h>

namespace jfsh {
namespace {

using test::Rig;
using test::TempDir;

// A completion widget on Tab running f; `got` is what f leaves.
struct CRig : Rig {
  CRig() : Rig(80) {
    sh.run("zle -C cw complete-word f; bindkey '^I' cw");
    // What compadd left, as zsh's $compstate has it.
    sh.run("U() { got=\"n=$compstate[nmatches] u=$compstate[unambiguous] "
           "c=$compstate[unambiguous_cursor] p=$compstate[unambiguous_positions]\" }");
  }
  str var(strv name) { return sh.run("print -rn -- \"$" + str(name) + "\"").out; }
  // Types `keys`, then a key that shows the line and the cursor.
  str run(const str& keys) {
    sh.run("got=");
    start();
    send(keys + "\x18\x04");
    settle();
    send("\x03");
    finish();
    return var("buf");
  }
  // Completes `word` after `echo ` with `body` as f: the line with the
  // cursor marked by `|`, and what U left.
  str complete(strv body, strv word, strv after = "") {
    sh.run("f() { " + str(body) + "; U }");
    str line = run("echo " + str(word) + str(after) + "\t");
    return line + " " + var("got");
  }
};

void bind_dump(CRig& t) {
  t.sh.run("dump() { buf=\"${LBUFFER}|${RBUFFER}\" }; zle -N dump; bindkey '^X^D' dump");
}

// ---- Match specifications ------------------------------------------------------------

TEST(Matcher, CaseInsensitive) {
  CRig t;
  bind_dump(t);
  // The unambiguous part comes from the matches; the cursor stops where
  // they differ.
  EXPECT_EQ(t.complete("compadd -M 'm:{a-z}={A-Z}' Foo foo FOO bar", "fo"),
            "echo f|oo n=3 u=foo c=2 p=1:3");
  EXPECT_EQ(t.complete("compadd -M 'm:{a-z}={A-Z}' Foo Fob", "fo"), "echo Fo| n=2 u=Fo c=3 p=2");
  EXPECT_EQ(t.complete("compadd -M 'm:{a-z}={A-Z}' Foobar", "fo"),
            "echo Foobar | n=1 u=Foobar c=7 p=6");
  // One way only: an upper-case letter on the line is itself.
  EXPECT_EQ(t.complete("compadd -M 'm:{a-z}={A-Z}' foo", "FO"), "echo FO| n=0 u= c=1 p=");
  EXPECT_EQ(t.complete("compadd -M 'm:{a-zA-Z}={A-Za-z}' foo Foo", "FO"),
            "echo F|oo n=2 u=Foo c=2 p=1:3");
  EXPECT_EQ(t.complete("compadd -M 'm:{[:lower:]}={[:upper:]}' FOOBAR FOOBAZ", "foo"),
            "echo FOOBA| n=2 u=FOOBA c=6 p=5");
  EXPECT_EQ(t.complete("compadd -M 'm:{a-z}={A-Z}' FooBar fooBaz", "fooba"),
            "echo fooBa| n=2 u=fooBa c=6 p=1:5");
  EXPECT_EQ(t.complete("compadd -M 'm:{a-z-}={A-Z_}' FOO_BAR FOO_BAZ", "foo-b"),
            "echo FOO_BA| n=2 u=FOO_BA c=7 p=6");
  EXPECT_EQ(t.complete("compadd -M 'm:{[:lower:][:upper:]}={[:upper:][:lower:]}' fOo FoO", "FO"),
            "echo F|Oo n=2 u=FOo c=2 p=1:3");
  EXPECT_EQ(t.complete("compadd -M 'm:?=?' abc xyz", "q"), "echo q|bc n=2 u=qbc c=2 p=1:3");
  // x: ends the specification.
  EXPECT_EQ(t.complete("compadd -M 'm:{a-z}={A-Z} x: r:|=*' Foo FooBar", "fo"),
            "echo Foo| n=2 u=Foo c=4 p=3");
  // Two -M add up.
  EXPECT_EQ(t.complete("compadd -M 'm:{a-z}={A-Z}' -M 'r:|=*' Foobar Fooqux", "fq"),
            "echo fq| n=0 u= c=1 p=");
}

TEST(Matcher, PartialWords) {
  CRig t;
  bind_dump(t);
  EXPECT_EQ(t.complete("compadd -M 'r:|[._-]=* r:|=*' comp.sources.unix comp.sys.unix comp.lang.c",
                       "c.s.u"),
            "echo comp.s|.unix n=2 u=comp.s.unix c=7 p=6:11");
  EXPECT_EQ(t.complete("compadd -M 'r:|.=*' comp.sources.unix", "..u"),
            "echo comp.sources.unix | n=1 u=comp.sources.unix c=18 p=17");
  EXPECT_EQ(t.complete("compadd -M 'r:|.=*' comp.sources.unix", ".u"), "echo .u| n=0 u= c=1 p=");
  EXPECT_EQ(t.complete("compadd -M 'r:|[._-]=* r:|=*' foo-bar-baz foo-bin-baz", "f-b-b"),
            "echo foo-b|-baz n=2 u=foo-b-baz c=6 p=5:9");
  EXPECT_EQ(t.complete("compadd -M 'r:|-=* r:|=*' foo-bar-baz foo-bin", "f-b"),
            "echo foo-b| n=2 u=foo-b c=6 p=5");
  // `*` doesn't match across an anchor, `**` does.
  EXPECT_EQ(t.complete("compadd -M 'r:|.=* r:|=*' a.b.c.d a.x.c.d", "a.c"),
            "echo a.c| n=0 u= c=1 p=");
  EXPECT_EQ(t.complete("compadd -M 'r:|.=** r:|=*' a.b.c.d a.x.c.d", "a.c"),
            "echo a.|.c.d n=2 u=a..c.d c=3 p=2:6");
  EXPECT_EQ(t.complete("compadd -M 'r:|=*' foobar foobaz", "fo"),
            "echo fooba| n=2 u=fooba c=6 p=5");
  t.sh.run("setopt completeinword");
  EXPECT_EQ(t.complete("compadd -M 'r:|.=* r:|=*' comp.sources.unix comp.sys.unix", "c.u", "\x02"),
            "echo comp.s|.unix n=2 u=comp.s.unix c=7 p=6:11");
}

TEST(Matcher, Substrings) {
  CRig t;
  bind_dump(t);
  EXPECT_EQ(t.complete("compadd -M 'l:|=* r:|=*' foobar barfoo xfoox other", "foo"),
            "echo foo| n=3 u=foo c=4 p=3");
  EXPECT_EQ(t.complete("compadd -M 'l:|=* r:|=*' abcdef xbcdy", "bcd"),
            "echo bcd| n=2 u=bcd c=4 p=0:3");
  EXPECT_EQ(t.complete("compadd -M 'l:|=* r:|=*' abcdef", "cd"),
            "echo abcdef | n=1 u=abcdef c=7 p=6");
  // The same text, the cursor goes to where the matches differ.
  EXPECT_EQ(t.complete("compadd -M 'l:|=*' xxfoo yyfoo", "foo"), "echo |foo n=2 u=foo c=1 p=0:3");
  EXPECT_EQ(t.complete("compadd -M 'l:|=* r:|=*' abc-def xyz-def", "def"),
            "echo |-def n=2 u=-def c=1 p=0:4");
}

TEST(Matcher, AnchorsAndLineForms) {
  CRig t;
  bind_dump(t);
  // Upper-case forms put the line's characters in the match.
  EXPECT_EQ(t.complete("compadd -M 'M:_=' foo", "f_o"), "echo f_oo | n=1 u=f_oo c=5 p=4");
  EXPECT_EQ(t.complete("compadd -M 'L:|-=' foo", "-f"), "echo -foo | n=1 u=-foo c=5 p=4");
  EXPECT_EQ(t.complete("compadd -M 'L:--|no-=' -- --foo", "--no-f"),
            "echo --no-foo | n=1 u=--no-foo c=9 p=8");
  EXPECT_EQ(t.complete("compadd -M 'b:-=+' +foo +fob", "-f"), "echo +fo| n=2 u=+fo c=4 p=3");
  EXPECT_EQ(t.complete("compadd -M 'B:0=' 12 13", "001"), "echo 001| n=2 u=001 c=4 p=3");
  EXPECT_EQ(t.complete("compadd -M 'B:[nN][oO]=' nomatch", "nom"),
            "echo nomatch | n=1 u=nomatch c=8 p=7");
  EXPECT_EQ(t.complete("compadd -M 'B:[nN][oO]=' match", "nonom"),
            "echo nonomatch | n=1 u=nonomatch c=10 p=9");
  EXPECT_EQ(t.complete("compadd -M 'e:.=,' foo,", "foo."), "echo foo.| n=0 u= c=1 p=");
  EXPECT_EQ(t.complete("compadd -M 'R:|.=' foo", "foo."), "echo foo.| n=0 u= c=1 p=");
  EXPECT_EQ(t.complete("compadd -M 'l:.|=*' foo.bar", ".bar"), "echo .bar| n=0 u= c=1 p=");
  // Two anchors.
  EXPECT_EQ(t.complete("compadd -M 'r:?||[[:upper:]]=*' fooBar fooHooBar", "fB"),
            "echo fooBar | n=1 u=fooBar c=7 p=6");
  EXPECT_EQ(t.complete("compadd -M 'r:?||[[:upper:]]=*' fooBar", "B"),
            "echo fooBar | n=1 u=fooBar c=7 p=6");
  EXPECT_EQ(t.complete("compadd -M 'L:.||[[:alpha:]]=by' pass.byname", "pass.n"),
            "echo pass.name | n=1 u=pass.name c=10 p=9");
}

TEST(Matcher, WithTheOtherOptions) {
  CRig t;
  bind_dump(t);
  // -O gets the words, -A the strings that go in the line, -D keeps the
  // elements of the words that match.
  EXPECT_EQ(t.complete("compadd -A arr -M 'M:_=' foo; got2=\"$arr\"", "f_o"),
            "echo f_o| n=0 u= c=1 p=");
  EXPECT_EQ(t.var("got2"), "f_oo");
  EXPECT_EQ(t.complete("compadd -O arr -M 'm:{a-z}={A-Z}' Foo foo Bar; got2=\"$arr\"", "fo"),
            "echo fo| n=0 u= c=1 p=");
  EXPECT_EQ(t.var("got2"), "Foo foo");
  EXPECT_EQ(t.complete("arr=(Foo foo Bar); compadd -D arr -M 'm:{a-z}={A-Z}' Foo foo Bar"
                       "; got2=\"$arr\"", "fo"),
            "echo fo| n=0 u= c=1 p=");
  EXPECT_EQ(t.var("got2"), "Foo foo");
  // -p, -s and -P with matchers.
  EXPECT_EQ(t.complete("compadd -p pre/ -M 'm:{a-z}={A-Z}' Foo Fab", "pre/f"),
            "echo pre/F| n=2 u=pre/F c=6 p=5");
  EXPECT_EQ(t.complete("compadd -p PRE/ -M 'm:{a-z}={A-Z}' Foo Fab", "pre/f"),
            "echo pre/f| n=2 u=PREF c=5 p=4");
  EXPECT_EQ(t.complete("compadd -s .txt -M 'm:{a-z}={A-Z}' Foo Fab", "f"),
            "echo F|o.txt n=2 u=Fo.txt c=2 p=1:2:6");
  EXPECT_EQ(t.complete("compadd -P pre -M 'm:{a-z}={A-Z}' Foo Fab", "pref"),
            "echo preF| n=2 u=preF c=5 p=4");
  // Matchers of one call don't apply to another's words, but the
  // unambiguous string is the matches' together.
  EXPECT_EQ(t.complete("compadd -M 'm:{a-z}={A-Z}' Foo; compadd foobar", "fo"),
            "echo foo| n=2 u=foo c=4 p=1:3");
  EXPECT_EQ(t.complete("compadd foobar; compadd -M 'm:{a-z}={A-Z}' FOOBAZ", "fo"),
            "echo fooba| n=2 u=fooba c=6 p=3:4:5");
  EXPECT_EQ(t.complete("compadd -U -M 'm:{a-z}={A-Z}' Foo foo", "fo"),
            "echo f|oo n=2 u=foo c=2 p=1:3");
  EXPECT_EQ(t.complete("compadd -M 'm:{a-z}={A-Z}' 'Foo bar' 'Foo baz'", "fo"),
            "echo Foo\\ ba| n=2 u=Foo\\ ba c=8 p=7");
  EXPECT_EQ(t.complete("compset -q; compadd -M 'm:{a-z}={A-Z}' Foo Fob", "\"fo"),
            "echo \"Fo| n=2 u=\"Fo c=4 p=3");
  EXPECT_EQ(t.complete("compset -P '*='; compadd -M 'm:{a-z}={A-Z}' Foo Fob", "x=fo"),
            "echo x=Fo| n=2 u=x=Fo c=5 p=4");
  // An exact match.
  EXPECT_EQ(t.complete("compadd -M 'm:{a-z}={A-Z}' Foo foo foobar; got2=$compstate[exact_string]",
                       "foo"),
            "echo foo| n=3 u=foo c=4 p=1:3");
  EXPECT_EQ(t.var("got2"), "foo");
}

TEST(Matcher, MenuAndListing) {
  CRig t;
  bind_dump(t);
  t.sh.run("setopt automenu");
  t.sh.run("f() { compadd -M 'm:{a-z}={A-Z}' Foo FOO fob }");
  EXPECT_EQ(t.run("echo fo\t\t"), "echo FOO|");
  EXPECT_EQ(t.run("echo fo\t\t\t"), "echo Foo|");
  t.sh.run("f() { compadd -M 'r:|.=* r:|=*' comp.sources.unix comp.sys.unix }");
  t.type("echo c.s.u\t\t");
  EXPECT_EQ(t.screen.row(1), "comp.sources.unix  comp.sys.unix");
  t.send("\x03");
  t.finish();
}

TEST(Matcher, BadSpecifications) {
  CRig t;
  TempDir dir;
  str err = (dir.path / "err").string();
  auto check = [&](strv spec) {
    t.sh.run("f() { compadd -M '" + str(spec) + "' foo 2>>" + err + "; rc=$? }");
    t.run("echo f\t");
    s::ifstream in(err);
    s::stringstream ss;
    ss << in.rdbuf();
    s::ofstream{err};   // empty it for the next one
    return ss.str() + t.var("rc");
  };
  EXPECT_EQ(check("q:x=y"), "jfsh: compadd: unknown match specification character `q'\n1");
  EXPECT_EQ(check("m:x"), "jfsh: compadd: missing word pattern\n1");
  EXPECT_EQ(check("m:[ab=y"), "jfsh: compadd: unterminated character class\n1");
  EXPECT_EQ(check("m:*=y"), "jfsh: compadd: invalid pattern character `*'\n1");
  EXPECT_EQ(check("m:a=*"), "jfsh: compadd: need anchor for `*'\n1");
  EXPECT_EQ(check("x:foo"), "jfsh: compadd: unexpected pattern following x: specification\n1");
  EXPECT_EQ(check("r:a"), "jfsh: compadd: missing right anchor\n1");
  EXPECT_EQ(check("l:a|"), "jfsh: compadd: missing line pattern\n1");
  EXPECT_EQ(check("m:="), "jfsh: compadd: need non-empty word or line pattern\n1");
  EXPECT_EQ(check("l:a||"), "jfsh: compadd: missing right anchor\n1");
}

// ---- Condition codes ---------------------------------------------------------------

TEST(CompletionConditions, PrefixSuffixAfterBetween) {
  CRig t;
  bind_dump(t);
  t.sh.run("setopt completeinword");
  t.sh.run("T() { local c; for c in \"$@\"; do eval \"[[ $c ]]\"; got+=\"$?\"; done }");
  auto conds = [&](strv line) {
    t.sh.run("f() { T '-prefix foo' '-prefix f' '-prefix f*' '-prefix 1 f*' '-prefix 3 f*'"
             " '-prefix 9 f*' '-prefix 0 f*' '-prefix *' '-prefix \"f*\"' '-suffix *'"
             " '-suffix b*' '-suffix 1 *r' '-suffix 2 *' '-suffix 0 *' '-after echo'"
             " '-after zz' '-after a*' '-between echo bar' '-between echo zz' '-between a* b*'"
             " '-between echo f*' '-between aa bb'; got+=\" $PREFIX $SUFFIX $CURRENT\" }");
    t.run(str(line) + "\t");
    return t.var("got");
  };
  EXPECT_EQ(conds("echo aa foobar bb\x02\x02\x02\x02\x02\x02"),
            "0000011010000101000010 foo bar 3");
  EXPECT_EQ(conds("echo aa "), "1111111010111101000000   3");
  EXPECT_EQ(conds("echo bar x foo bb aa\x02\x02\x02\x02\x02\x02"),
            "0000011010111101110111 foo  4");
  EXPECT_EQ(conds("echo fff"), "1000011010111101100111 fff  2");
}

TEST(CompletionConditions, CountIsArithmetic) {
  CRig t;
  t.sh.run("f() { local n=2; got=; [[ -prefix n f* ]]; got+=$?; [[ -prefix 1+1 f* ]]; got+=$?"
           "; [[ -prefix 2+2 'fo*' ]]; got+=$?; [[ -suffix n *o ]]; got+=$?"
           "; [[ -suffix 1+2 *o ]]; got+=$? }");
  t.run("echo fofo\t");
  EXPECT_EQ(t.var("got"), "00111");
}

TEST(CompletionConditions, ParsedAsZshDoes) {
  test::Shell sh;
  EXPECT_EQ(sh.run("f() { [[ -prefix -1 f* ]] }").err, "jfsh:1: parse error near `f*'\n");
  EXPECT_EQ(sh.run("f() { [[ -prefix -1 a b ]] }").err, "jfsh:1: parse error near `a'\n");
  EXPECT_EQ(sh.run("f() { [[ -prefix -1 ]] }; which f").out, "f () {\n\t[[ -prefix -1 ]]\n}\n");
}

TEST(CompletionConditions, OnlyInCompletionFunctions) {
  test::Shell sh;
  auto r = sh.run("[[ -prefix x ]]; echo rc=$?");
  EXPECT_EQ(r.err, "jfsh: condition can only be used in completion function\n");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(sh.run("[[ -foo x y ]]").err, "jfsh: unknown condition: -foo\n");
  EXPECT_EQ(sh.run("f() { [[ -prefix x y z ]] }; f").err, "jfsh: unknown condition: -prefix\n");
  EXPECT_EQ(sh.run("f() { [[ -prefix 2 x* ]] && [[ -between a b ]] }; which f").out,
            "f () {\n\t[[ -prefix 2 x* ]] && [[ -between a b ]]\n}\n");
}

// ---- zsh/computil -------------------------------------------------------------------

TEST(Computil, LoadsAsAModule) {
  test::Shell sh;
  EXPECT_EQ(sh.run("zmodload zsh/computil; echo $?").out, "0\n");
  EXPECT_EQ(sh.run("whence -w comptags comptry compdescribe compvalues compfiles compgroups").out,
            "comptags: builtin\ncomptry: builtin\ncompdescribe: builtin\ncompvalues: builtin\n"
            "compfiles: builtin\ncompgroups: builtin\n");
  for (strv b : {"comptags -i x", "comptry a", "compdescribe -i a b", "compvalues -i a b",
                 "compfiles -r a b", "compgroups a"})
    EXPECT_EQ(sh.run(str(b)).err, "jfsh: " + str(b.substr(0, b.find(' ')))
                                    + ": can only be called from completion function\n");
}

TEST(Computil, ComptagsAndComptry) {
  CRig t;
  auto tags = [&](strv body) {
    t.sh.run("P() { got+=\"$*;\" }; f() { " + str(body) + " }");
    t.run("echo x\t");
    return t.var("got");
  };
  // Nothing registered yet.
  EXPECT_EQ(tags("{ comptags -C v; P C=$?; comptry a; P try=$? } 2>/dev/null"), "C=1;try=1;");
  EXPECT_EQ(tags("comptags -i ctx a b c d; P i=$?; comptags -C v; P $v; comptags -T; P T=$?"
                 "; comptry a b; comptry c; comptags -T; P T=$?; comptags -S arr; P $arr"
                 "; comptags -N; P N=$?; comptags -S arr; P $arr; comptags -R a; P Ra=$?"
                 "; comptags -N; comptags -S arr; P $arr; comptags -R c; P Rc=$?"
                 "; comptags -N; P N=$?; comptags -T; P T=$?"),
            "i=0;ctx;T=1;T=0;a b;N=0;a b;Ra=0;c;Rc=0;N=1;T=1;");
  EXPECT_EQ(tags("comptags -i ctx a b c; comptry -s a b c; comptags -N; comptags -S arr; P $arr"
                 "; comptags -N; comptags -S arr; P $arr; comptags -N; P N=$?"),
            "a;b;N=0;");
  EXPECT_EQ(tags("comptags -i ctx a b c; comptry a:x a:y b zz; comptags -N; comptags -S arr"
                 "; P ${(j:,:)arr}; comptags -A a v w; P $? $v $w; comptags -A a v w"
                 "; P $? $v $w; comptags -A a v w; P $?; comptags -A b v; P $? $v"),
            "a:x,a:y,b;0 x a:x;0 y a:y;1;0 b;");
  EXPECT_EQ(tags("comptags -i ctx files dirs other; comptry -m 'f* d*:desc'; comptags -N"
                 "; comptags -S arr; P ${(j:,:)arr}; comptry -m '*'; comptags -N"
                 "; comptags -S arr; P ${(j:,:)arr}; comptags -N; P N=$?"),
            "files,dirs:desc;files,dirs,other;N=1;");
  // -m: each argument's tags add to the list of those before.
  EXPECT_EQ(tags("comptags -i ctx a-b c; comptry -m 'a\\-*' 'c'; comptags -N; comptags -S arr"
                 "; P ${(j:,:)arr}; comptags -N; comptags -S arr; P ${(j:,:)arr}"),
            "a-b;a-b,c;");
  EXPECT_EQ(tags("comptags -i ctx a b; comptry a:-x 'a:y z'; comptags -N; comptags -A a v w"
                 "; P $v $w; comptags -A a v w; P $v $w"),
            "a-x a:-x;y z a:y z;");
  // One set of tags per function level; -S- etc. look one up.
  EXPECT_EQ(tags("g() { comptags -i inner p q; comptry q; comptags -N; comptags -S a1; P $a1"
                 "; comptags -S- a2; P $a2 }; comptags -i outer a b; comptry b a; comptags -N"
                 "; g; comptags -S a3; P $a3"),
            "q;b a;b a;");
}

TEST(Computil, Compdescribe) {
  CRig t;
  t.sh.run("G() { while compdescribe -g csl o m d; do got+=\"$csl (${(j:|:)o}) (${(j:|:)m}) "
           "(${(j:|:)d});\"; done }");
  auto desc = [&](strv body) {
    t.sh.run("f() { " + str(body) + "; G }");
    t.run("echo x\t");
    return t.var("got");
  };
  EXPECT_EQ(desc("a=(foo:'the foo' bar:'the bar' baz); compdescribe -i '' 30 a"),
            " () (foo|bar|baz) (foo|bar|baz);");
  EXPECT_EQ(desc("a=(foo:'the foo' bar:'the bar' baz); ex=(-J grp -X expl)"
                 "; compdescribe -I '' 30 ' -- ' ex a"),
            " (-l|-J|grp|-X|expl) (foo|bar) (foo   -- the foo|bar   -- the bar); "
            "(-J|grp|-X|expl) (baz) (baz);");
  // Matches with the same description are grouped (-g) on one line.
  str pad(60, ' ');
  EXPECT_EQ(desc("a=(foo:'same' bar:'same' baz:'other' qux); ex=(-J grp)"
                 "; compdescribe -I '' 30 ' -- ' ex -g a"),
            "packed (-2V|grp) (bar) (bar);packed (-2V|grp) (baz) (baz);packed (-2V|grp) (foo) "
            "(foo);packed (-E1|-J|grp) () ();packed (-E2|-J|grp) () ( -- same" + pad
                + "| -- other" + str(59, ' ') + "); (-J|grp) (qux) (qux);");
  // Hiding the leading - or -- of options (zsh then loses the
  // description).
  EXPECT_EQ(desc("a=(-a:'opt a' --bee:'opt b' +c:'opt c'); ex=()"
                 "; compdescribe -I '-' 30 ' -- ' ex a"),
            " (-l) (-a|--bee|+c) (a|-bee|c);");
  EXPECT_EQ(desc("a=(foo:'d1' bar:'d2'); m=(FOO BAR); ex=()"
                 "; compdescribe -I '' 30 ' -- ' ex a m -S x --"),
            " (-l|-S|x) (FOO|BAR) (foo   -- d1|bar   -- d2);");
  EXPECT_EQ(desc("a=('f\\:oo:has colon' 'x:a\\:b'); ex=(); compdescribe -I '' 30 ' -- ' ex a"),
            " (-l) (f:oo|x) (f:oo   -- has colon|x      -- a:b);");
  EXPECT_EQ(desc("ex=(); compdescribe -I '' 30 ' -- ' ex '(p\\:1 q:d2)'"),
            " (-l) (p|q) (p   -- 1|q   -- d2);");
  EXPECT_EQ(desc("{ compdescribe -g a b c d; got+=$?; compdescribe -x; got+=$?"
                 "; compdescribe -I a b c d; got+=$? } 2>/dev/null; got+=\";\""),
            "111;");
}

TEST(Computil, Compvalues) {
  CRig t;
  t.sh.run("S() { local d a c s S dd na ar op; typeset -A v; compvalues -D d a; got+=\"D$? $d $a;\""
           "; compvalues -C c; got+=\"C$? $c;\"; compvalues -V na ar op"
           "; got+=\"V$? (${(j:|:)na}) (${(j:|:)ar}) (${(j:|:)op});\"; compvalues -s s"
           "; got+=\"s$? $s;\"; compvalues -S S; got+=\"S$? $S;\"; compvalues -d dd"
           "; got+=\"d$? $dd;\"; compvalues -v v; got+=\"v$?\"; for k in ${(ok)v}; do"
           " got+=\" $k=$v[$k]\"; done; got+=\";$IPREFIX|$PREFIX\" }");
  str spec = "'desc' 'foo[the foo]' 'bar[the bar]:barval:(x y)'"
             " '(foo)baz[conflicts]::bazval:_files' '*multi'";
  auto values = [&](strv opts, strv word) {
    t.sh.run("f() { compvalues -i " + str(opts) + " " + spec + "; got=\"i$?;\"; S }");
    t.run("echo " + str(word) + "\t");
    return t.var("got");
  };
  EXPECT_EQ(values("", ""), "i0;D1  ;C1 ;V0 (foo:the foo|multi) (bar:the bar) (baz:conflicts);"
                            "s1 ;S0 =;d0 desc;v0;|");
  EXPECT_EQ(values("-s ,", "foo,b"), "i0;D1  ;C1 ;V0 (multi) (bar:the bar) (baz:conflicts);"
                                     "s0 ,;S0 =;d0 desc;v0 foo=;foo,|b");
  EXPECT_EQ(values("-s ,", "foo,bar=x"), "i0;D0 barval (x y);C0 bar;V0 (multi) (bar:the bar) "
                                         "(baz:conflicts);s0 ,;S0 =;d0 desc;v0 bar=x foo=;"
                                         "foo,bar=|x");
  EXPECT_EQ(values("-s ,", "baz=,fo"), "i0;D1  ;C1 ;V0 (multi) (bar:the bar) ();s0 ,;S0 =;"
                                       "d0 desc;v0 baz=;baz=,|fo");
  EXPECT_EQ(values("-s , -S :", "bar:q,foo,mu"), "i0;D1  ;C1 ;V0 (multi) () (baz:conflicts);"
                                                 "s0 ,;S0 :;d0 desc;v0 bar=q foo=;bar:q,foo,|mu");
  EXPECT_EQ(values("-w -s ,", "foo bar=1 ba"), "i0;D1  ;C1 ;V0 (multi) () (baz:conflicts);s0 ,;"
                                               "S0 =;d0 desc;v0 bar=1 foo=;|ba");
  t.sh.run("f() { compvalues -i " + spec + "; compvalues -L bar d a n"
           "; got=\"$? $d $a $n\"; compvalues -L foo d a; got+=\" $?\" }");
  t.run("echo \t");
  EXPECT_EQ(t.var("got"), "0 barval (x y) bar 1");
  TempDir dir;
  str err = (dir.path / "err").string();
  str e = " 2>>" + err + "; got+=$?; ";
  t.sh.run("f() { got=; compvalues -i" + e + "compvalues -i d" + e + "compvalues -i d '(a'" + e
           + "compvalues -i d 'a[x'" + e + "compvalues -i -s '' d ab" + e
           + "compvalues -i -s '' d a:x" + e + "compvalues -i d 'a]'" + e + "}");
  t.run("echo \t");
  EXPECT_EQ(t.var("got"), "1111110");
  s::ifstream in(err);
  s::stringstream ss;
  ss << in.rdbuf();
  EXPECT_EQ(ss.str(), "jfsh: compvalues: not enough arguments\n"
                      "jfsh: compvalues: not enough arguments\n"
                      "jfsh: compvalues: invalid argument: (a\n"
                      "jfsh: compvalues: invalid value definition: a[x\n"
                      "jfsh: compvalues: no multi-letter values with empty separator allowed\n"
                      "jfsh: compvalues: no value with argument with empty separator allowed\n");
}

TEST(Computil, Compfiles) {
  CRig t;
  TempDir dir;
  t.sh.run("cd " + dir.path.string() + "; touch exactfile; mkdir -p igd/sub");
  auto pats = [&](strv args, strv word) {
    t.sh.run("f() { t=('' 'a b/'); ac=(); fk=('.:fakeA fakeB'); " + str(args)
             + "; got=\"$? ${(j:|:)t}\" }");
    t.run("echo " + str(word) + "\t");
    return t.var("got");
  };
  EXPECT_EQ(pats("compfiles -p t ac '' '' '' fk '*' '*.c'", "fo"),
            "0 fo*|fo*.c|a b/fo*|a b/fo*.c|fakeA|fakeB");
  EXPECT_EQ(pats("compfiles -P t ac 'sub/' '' '' fk", "fo"),
            "0 sub/fo*(-/)|a b/sub/fo*(-/)|sub/fakeA|sub/fakeB");
  EXPECT_EQ(pats("t=(''); compfiles -p t ac '' 'm:{a-z}={A-Z}' '' fk '*'", "fo"),
            "0 [fF][oO]*|fakeA|fakeB");
  EXPECT_EQ(pats("t=(''); compfiles -p t ac '' 'r:|.=* r:|=*' '' fk '*'", "c.s"),
            "0 c*.s*|fakeA|fakeB");
  EXPECT_EQ(pats("t=(''); compfiles -p- t ac '' 'm:{a-z}={A-Z}' '' fk '*'", "fo"),
            "0 *|fakeA|fakeB");
  EXPECT_EQ(pats("t=(''); fk=(); compfiles -p t ac '' '' 'yes' fk '*'", ".f"), "0 .f*|..|.");
  EXPECT_EQ(pats("t=(''); fk=(); compfiles -p t ac '' '' '..' fk '*'", "."), "0 .*|..");
  EXPECT_EQ(pats("t=(''); fk=(); ac=(true); compfiles -p t ac '' '' '' fk '*'", "exactfile"),
            "0 exactfile");
  EXPECT_EQ(pats("t=(''); fk=(); compfiles -p t ac '' 'm:{a-z}={A-Z} r:|[._-]=*' '' fk '*'",
                 "ab.c"),
            "0 [aA][bB]*.[cC]*");
  EXPECT_EQ(pats("t=(''); fk=(); compfiles -p t ac '' 'l:|=* r:|=*' '' fk '*'", "ab"), "0 *");
  EXPECT_EQ(pats("t=(''); fk=(); compfiles -p t ac '' 'm:{a-zA-Z}={A-Za-z}' '' fk '*'", "aB_"),
            "0 [aA][Bb]_*");
  EXPECT_EQ(pats("t=(''); fk=(); compfiles -p t ac '' '' '' fk '*' 'x*'", "fo\\\\*"),
            "0 fo\\**|x*");
  // -r: only the names in the directory the prefix names; 1 when they
  // differ.
  t.sh.run("f() { got=; for p in 'foo/bar foo/baz:foo/b' 'foo/bar qux/baz:x/b'"
           " 'foo/bar foo/baz:x/b' 'foo foo:x' 'foo bar:x'; do t=(${=p%:*}); compfiles -r t ${p#*:}"
           "; got+=\"$? ${(j:|:)t};\"; done }");
  t.run("echo x\t");
  EXPECT_EQ(t.var("got"), "0 foo/bar|foo/baz;1 foo/bar|qux/baz;0 foo/bar|foo/baz;0 foo|foo;"
                          "1 foo|bar;");
  // -i: the current directory and the path's parents are ignored.
  t.sh.run("f() { t=(./ igd/sub/.. igd); ig=(old); compfiles -i t ig 'parent pwd' ''"
           "; got=\"$? ${(j:|:)ig}\" }");
  t.run("echo x\t");
  EXPECT_EQ(t.var("got"), "0 old|./|igd/sub/..");
}

TEST(Computil, CompgroupsOrdersGroups) {
  CRig t;
  auto listing = [&](strv body) {
    t.sh.run("f() { " + str(body) + "; compstate[insert]=''; compstate[list]=list }");
    t.type("echo \t");
    str rows;
    for (int y = 1; y < 6 and not t.screen.row(y).empty(); ++y) rows += t.screen.row(y) + ";";
    t.send("\x03");
    t.finish();
    return rows;
  };
  EXPECT_EQ(listing("compgroups grp; compadd -J grp b a; compadd -V grp z y; compadd -2V grp q q"
                    "; compadd -1V grp w w"),
            "q  q;z  y;a  b;w;");
  EXPECT_EQ(listing("compadd -V grp z y; compgroups grp; compadd -J grp b a; compadd -2V grp q q"),
            "z  y;q  q;a  b;");
  EXPECT_EQ(listing("compgroups g1 g2; compadd -J g2 b a; compadd -J g1 z y"), "y  z;a  b;");
}

}  // namespace
}  // namespace jfsh
