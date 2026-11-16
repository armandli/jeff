#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <jfsh/core/aliases.h>
#include <jfsh/core/param.h>

namespace jfsh {
namespace {

s::ptrdiff_t count_prefix(const vec<str>& env, strv prefix) {
  return s::count_if(env.begin(), env.end(),
                     [&](const str& e) { return e.starts_with(prefix); });
}

// make_envp emits each name once, with its innermost value (bug B12).
TEST(ParamTable, EnvpUsesTheInnermostDefinitionOnce) {
  ParamTable params;
  params.assign("X", str("outer"), PM_SCALAR | PM_EXPORTED);
  params.push_scope();
  params.assign("X", str("inner"), PM_SCALAR | PM_EXPORTED);
  auto env = params.make_envp();
  EXPECT_EQ(count_prefix(env, "X="), 1);
  EXPECT_NE(s::ranges::find(env, str("X=inner")), env.end());
}

TEST(ParamTable, UnexportedLocalHidesAnExportedGlobal) {
  ParamTable params;
  params.assign("X", str("outer"), PM_SCALAR | PM_EXPORTED);
  params.push_scope();
  params.assign("X", str("inner"), PM_SCALAR);
  EXPECT_EQ(count_prefix(params.make_envp(), "X="), 0);
}

TEST(ParamTable, IntegersAreExportedAsText) {
  ParamTable params;
  params.assign("N", s::int64_t{42}, PM_INTEGER | PM_EXPORTED);
  EXPECT_EQ(params.make_envp(), vec<str>{"N=42"});
}

TEST(ParamTable, SetScalarKeepsAttributesAndCreatesGlobals) {
  ParamTable params;
  params.assign("E", str("1"), PM_SCALAR | PM_EXPORTED);
  params.push_scope();
  ASSERT_TRUE(params.set_scalar("E", "2"));
  ASSERT_TRUE(params.set_scalar("NEW", "v"));
  params.pop_scope();
  EXPECT_EQ(params.lookup("E")->get().scalar(), "2");
  EXPECT_TRUE(params.lookup("E")->get().is_exported());
  EXPECT_EQ(params.lookup("NEW")->get().scalar(), "v");
}

TEST(ParamTable, SetScalarRefusesReadonly) {
  ParamTable params;
  params.assign("R", str("x"), PM_SCALAR | PM_READONLY);
  auto r = params.set_scalar("R", "y");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().message, "read-only variable: R");
  EXPECT_EQ(params.lookup("R")->get().scalar(), "x");
}

// The global scope survives an unbalanced pop, even when push_scope created it.
TEST(ParamTable, GlobalScopeIsNeverPopped) {
  ParamTable params;
  params.push_scope();
  params.pop_scope();
  params.pop_scope();
  ASSERT_TRUE(params.set_scalar("G", "1"));
  EXPECT_EQ(params.current_scope(), 0);
  EXPECT_EQ(params.lookup("G")->get().scalar(), "1");
}

}  // namespace
}  // namespace jfsh
