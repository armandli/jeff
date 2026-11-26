#include <jfsh/modules/langinfo.h>

#include <langinfo.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

// zsh/langinfo (M11 Phase 11a): `$langinfo` associative array over the
// nl_langinfo(3) table.  The getter re-queries the C library on every
// lookup so a change to `$LC_CTYPE` / `setlocale()` is immediately
// visible.

namespace jfsh {

namespace {

struct Item { const char* name; nl_item code; };

// A broad selection of the items POSIX names; the platform decides which
// are populated.  Missing ones map to the empty string.
#define JFSH_LANGINFO_TABLE                                             \
  X(CODESET)                                                            \
  X(D_T_FMT) X(D_FMT) X(T_FMT) X(T_FMT_AMPM)                             \
  X(AM_STR) X(PM_STR) X(ERA) X(ERA_D_FMT) X(ERA_D_T_FMT) X(ERA_T_FMT)   \
  X(ALT_DIGITS)                                                         \
  X(RADIXCHAR) X(THOUSEP) X(YESEXPR) X(NOEXPR)                          \
  X(CRNCYSTR)                                                           \
  X(DAY_1) X(DAY_2) X(DAY_3) X(DAY_4) X(DAY_5) X(DAY_6) X(DAY_7)        \
  X(ABDAY_1) X(ABDAY_2) X(ABDAY_3) X(ABDAY_4)                           \
  X(ABDAY_5) X(ABDAY_6) X(ABDAY_7)                                      \
  X(MON_1) X(MON_2) X(MON_3) X(MON_4) X(MON_5) X(MON_6)                 \
  X(MON_7) X(MON_8) X(MON_9) X(MON_10) X(MON_11) X(MON_12)              \
  X(ABMON_1) X(ABMON_2) X(ABMON_3) X(ABMON_4) X(ABMON_5) X(ABMON_6)     \
  X(ABMON_7) X(ABMON_8) X(ABMON_9) X(ABMON_10) X(ABMON_11) X(ABMON_12)

static const vec<Item>& table() {
  static const vec<Item> t = []{
    vec<Item> v;
#define X(n) v.push_back({#n, n});
    JFSH_LANGINFO_TABLE
#undef X
    return v;
  }();
  return t;
}

}  // namespace

void register_langinfo_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/langinfo", [](ModuleFeatures& out, ExecContext& ctx) {
    Param& p = ctx.params.assign("langinfo", umap<str, str>{},
                                 PM_HASH | PM_SPECIAL | PM_READONLY);
    p.handlers = s::make_unique<ParamHandlers>();
    p.handlers->get_hash = []() {
      umap<str, str> h;
      for (const auto& e : table()) {
        const char* v = ::nl_langinfo(e.code);
        h[e.name] = v ? v : "";
      }
      return h;
    };
    out.params.emplace_back("langinfo");
  });
}

}  // namespace jfsh
