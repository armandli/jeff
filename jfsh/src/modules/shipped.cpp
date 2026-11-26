// The modules jfsh ships, and the autoload marks and dependencies a shell
// starts with: zsh's bltinmods.list, as zsh 5.9 is built with them
// (`zsh -f -c 'zmodload -LFa; zmodload -Ld'`).

#include <jfsh/builtins/builtins.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/attr.h>
#include <jfsh/modules/cap.h>
#include <jfsh/modules/clone.h>
#include <jfsh/modules/compctl.h>
#include <jfsh/modules/computil.h>
#include <jfsh/modules/curses.h>
#include <jfsh/modules/datetime.h>
#include <jfsh/modules/db_gdbm.h>
#include <jfsh/modules/deltochar.h>
#include <jfsh/modules/example.h>
#include <jfsh/modules/files.h>
#include <jfsh/modules/langinfo.h>
#include <jfsh/modules/mapfile.h>
#include <jfsh/modules/mathfunc.h>
#include <jfsh/modules/module.h>
#include <jfsh/modules/nearcolor.h>
#include <jfsh/modules/net_socket.h>
#include <jfsh/modules/net_tcp.h>
#include <jfsh/modules/newuser.h>
#include <jfsh/modules/parameter.h>
#include <jfsh/modules/pcre.h>
#include <jfsh/modules/private.h>
#include <jfsh/modules/regex.h>
#include <jfsh/modules/stat.h>
#include <jfsh/modules/system.h>
#include <jfsh/modules/termcap.h>
#include <jfsh/modules/terminfo.h>
#include <jfsh/modules/watch.h>
#include <jfsh/modules/zftp.h>
#include <jfsh/modules/zleparameter.h>
#include <jfsh/modules/zprof.h>
#include <jfsh/modules/zpty.h>
#include <jfsh/modules/zselect.h>
#include <jfsh/modules/zutil.h>

namespace jfsh {

namespace {

// zsh's modules whose builtins jfsh keeps with the rest of the shell: the
// module brings them in (and $builtins says "undefined" until it does).
void register_core_modules(ModuleManager& mm) {
  // zsh/main is the shell itself, always loaded, without features.
  mm.register_builtin_module("zsh/main", [](ModuleFeatures& out, ExecContext&) {
    out.no_features = true;
  });
  mm.register_builtin_module("zsh/zle", [](ModuleFeatures& out, ExecContext&) {
    out.builtins = {{"bindkey", builtin_bindkey, BINF_NONE},
                    {"vared",   builtin_vared,   BINF_NONE},
                    {"zle",     builtin_zle,     BINF_NONE}};
  });
  mm.register_builtin_module("zsh/complete", [](ModuleFeatures& out, ExecContext&) {
    out.builtins = {{"compadd", builtin_compadd, BINF_NONE},
                    {"compset", builtin_compset, BINF_NONE}};
    for (auto [name, lo, hi] : {s::tuple{"after", 1, 1}, s::tuple{"between", 2, 2},
                                s::tuple{"prefix", 1, 2}, s::tuple{"suffix", 1, 2}}) {
      CondDef c;
      c.name      = name;
      c.infix     = false;
      c.min_args  = lo;
      c.max_args  = hi;
      c.prefix_fn = [op = "-" + str(name)](const vec<str>& args, ExecContext& ctx) {
        return cond_completion(op, args, ctx);
      };
      out.conds.push_back(s::move(c));
    }
  });
  mm.register_builtin_module("zsh/complist", [](ModuleFeatures&, ExecContext&) {});
  mm.register_builtin_module("zsh/rlimits", [](ModuleFeatures& out, ExecContext&) {
    out.builtins = {{"ulimit", builtin_ulimit, BINF_NONE}};
  });
  mm.register_builtin_module("zsh/sched", [](ModuleFeatures& out, ExecContext& ctx) {
    out.builtins = {{"sched", builtin_sched, BINF_NONE}};
    // $zsh_scheduled_events: the events as `time:flags:command`.
    Param& p = ctx.params.assign("zsh_scheduled_events", vec<str>{},
                                 PM_ARRAY | PM_READONLY | PM_SPECIAL | PM_HIDE | PM_HIDEVAL);
    p.handlers = s::make_unique<ParamHandlers>();
    ExecContext* c = &ctx;
    p.handlers->get_array = [c] {
      vec<str> v;
      for (const auto& e : c->loop.sched)
        v.push_back(s::to_string(static_cast<long long>(e.when)) + (e.trash_zle ? ":-o:" : "::")
                    + e.cmd);
      return v;
    };
    out.params = {"zsh_scheduled_events"};
  });
}

}  // namespace

void setup_shipped_modules(ExecContext& ctx) {
  ModuleManager& mm = ctx.modules;
  register_core_modules(mm);
  register_datetime_module    (mm);
  register_mathfunc_module    (mm);
  register_parameter_module   (mm);
  register_zutil_module       (mm);
  register_computil_module    (mm);
  register_system_module      (mm);
  register_files_module       (mm);
  register_stat_module        (mm);
  register_pcre_module        (mm);
  register_regex_module       (mm);
  register_example_module     (mm);
  register_langinfo_module    (mm);
  register_zselect_module     (mm);
  register_net_socket_module  (mm);
  register_zleparameter_module(mm);
  register_net_tcp_module     (mm);
  register_zftp_module        (mm);
  register_termcap_module     (mm);
  register_terminfo_module    (mm);
  register_curses_module      (mm);
  register_nearcolor_module   (mm);
  register_mapfile_module     (mm);
  register_db_gdbm_module     (mm);
  register_private_module     (mm);
  register_zpty_module        (mm);
  register_zprof_module       (mm);
  register_attr_module        (mm);
  register_cap_module         (mm);
  register_deltochar_module   (mm);
  register_compctl_module     (mm);
  register_clone_module       (mm);
  register_watch_module       (mm);
  register_newuser_module     (mm);

  // zsh's autoloadable features, for those jfsh's modules have.  The
  // conditions go in the order that gives zsh's `zmodload -ac` listing.
  mm.autoload_features("zsh/compctl", {"b:compctl"});
  mm.autoload_features("zsh/complete", {"b:compadd", "b:compset", "c:prefix", "c:suffix",
                                        "c:between", "c:after"});
  mm.autoload_features("zsh/computil", {"b:comparguments", "b:compdescribe", "b:compfiles",
                                        "b:compgroups", "b:compquote", "b:comptags",
                                        "b:comptry", "b:compvalues"});
  mm.autoload_features("zsh/param/private", {"b:private"});
  mm.autoload_features("zsh/parameter", {
      "p:aliases", "p:builtins", "p:commands", "p:dirstack", "p:dis_aliases",
      "p:dis_builtins", "p:dis_functions", "p:dis_functions_source", "p:dis_galiases",
      "p:dis_patchars", "p:dis_reswords", "p:dis_saliases", "p:funcfiletrace",
      "p:funcsourcetrace", "p:funcstack", "p:functions", "p:functions_source", "p:functrace",
      "p:galiases", "p:history", "p:historywords", "p:jobdirs", "p:jobstates", "p:jobtexts",
      "p:modules", "p:nameddirs", "p:options", "p:parameters", "p:patchars", "p:reswords",
      "p:saliases", "p:userdirs", "p:usergroups"});
  mm.autoload_features("zsh/rlimits", {"b:ulimit"});
  mm.autoload_features("zsh/sched", {"b:sched", "p:zsh_scheduled_events"});
  mm.autoload_features("zsh/termcap", {"b:echotc", "p:termcap"});
  mm.autoload_features("zsh/terminfo", {"b:echoti", "p:terminfo"});
  mm.autoload_features("zsh/watch", {"b:log"});
  mm.autoload_features("zsh/zle", {"b:bindkey", "b:vared", "b:zle"});
  mm.autoload_features("zsh/zleparameter", {"p:keymaps", "p:widgets"});
  mm.autoload_features("zsh/zutil", {"b:zformat", "b:zparseopts", "b:zregexparse",
                                     "b:zstyle"});
  using Deps = s::pair<const char*, vec<str>>;
  for (const auto& [mod, deps] : {Deps{"zsh/compctl",      {"zsh/complete", "zsh/zle"}},
                                  Deps{"zsh/complete",     {"zsh/zle"}},
                                  Deps{"zsh/complist",     {"zsh/complete", "zsh/zle"}},
                                  Deps{"zsh/computil",     {"zsh/complete", "zsh/zle"}},
                                  Deps{"zsh/zleparameter", {"zsh/zle"}},
                                  Deps{"zsh/zutil",        {"zsh/complete"}}})
    for (const auto& d : deps) mm.add_dep(mod, d);
  (void)mm.require("zsh/main");
}

}  // namespace jfsh
