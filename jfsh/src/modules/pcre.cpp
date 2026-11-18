#include <jfsh/modules/pcre.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef JFSH_HAVE_PCRE2
# include <pcre2.h>
#endif

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

// zsh/pcre (M10 Phase 2c): bindings for PCRE2.  Only built when PCRE2
// headers are available (JFSH_HAVE_PCRE2 defined by CMake).  Without
// PCRE2, the builtins register but print "pcre: not built in".
//
// State: a single compiled pattern lives in a static (zsh keeps one too,
// via its usual module-local heap).  Multi-pattern state is a zsh
// extension that lands with the completion-side wiring work.

namespace jfsh {

namespace {

int err(strv cmd, strv msg) {
  s::fprintf(stderr, "jfsh: %.*s: %.*s\n",
             static_cast<int>(cmd.size()), cmd.data(),
             static_cast<int>(msg.size()), msg.data());
  return 1;
}

#ifdef JFSH_HAVE_PCRE2

// One compiled pattern at a time, matching zsh.
struct PcreState {
  pcre2_code* code = nullptr;
  ~PcreState() { if (code) pcre2_code_free(code); }
};

static PcreState& state() { static PcreState s; return s; }

int builtin_pcre_compile(vec<str>& args, ExecContext&) {
  uint32_t options = 0;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a.size() < 2 or a[0] != '-' or a == "--") { if (a == "--") ++i; break; }
    for (s::size_t k = 1; k < a.size(); ++k) {
      switch (a[k]) {
        case 'a': options |= PCRE2_ANCHORED; break;
        case 'i': options |= PCRE2_CASELESS; break;
        case 'm': options |= PCRE2_MULTILINE; break;
        case 's': options |= PCRE2_DOTALL; break;
        case 'x': options |= PCRE2_EXTENDED; break;
        default:
          s::fprintf(stderr, "jfsh: pcre_compile: bad option: -%c\n", a[k]);
          return 1;
      }
    }
  }
  if (i >= args.size()) return err("pcre_compile", "not enough arguments");
  const str& pat = args[i];
  int errcode = 0;
  PCRE2_SIZE erroffset = 0;
  pcre2_code* code = pcre2_compile(
      reinterpret_cast<PCRE2_SPTR>(pat.c_str()), pat.size(),
      options, &errcode, &erroffset, nullptr);
  if (not code) {
    PCRE2_UCHAR msg[256];
    pcre2_get_error_message(errcode, msg, sizeof msg);
    char buf[320];
    s::snprintf(buf, sizeof buf, "failed to compile regexp '%s' at offset %zu: %s",
                pat.c_str(), static_cast<size_t>(erroffset),
                reinterpret_cast<const char*>(msg));
    return err("pcre_compile", buf);
  }
  auto& st = state();
  if (st.code) pcre2_code_free(st.code);
  st.code = code;
  return 0;
}

int builtin_pcre_study(vec<str>&, ExecContext&) {
  // PCRE2 compiles with JIT on demand; nothing to do here.
  return state().code ? 0 : err("pcre_study", "no pattern compiled");
}

int builtin_pcre_match(vec<str>& args, ExecContext& ctx) {
  str var_name, arr_name;
  int offset = 0;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-v") {
      if (++i >= args.size()) return err("pcre_match", "option -v requires an argument");
      var_name = args[i];
    } else if (a == "-a") {
      if (++i >= args.size()) return err("pcre_match", "option -a requires an argument");
      arr_name = args[i];
    } else if (a == "-n") {
      if (++i >= args.size()) return err("pcre_match", "option -n requires an argument");
      offset = static_cast<int>(s::strtol(args[i].c_str(), nullptr, 10));
    } else if (a == "--") { ++i; break; }
    else if (a.size() > 1 and a[0] == '-') return err("pcre_match", "bad option: " + a);
    else break;
  }
  if (i >= args.size()) return err("pcre_match", "not enough arguments");
  if (not state().code)
    return err("pcre_match", "no pattern compiled (use pcre_compile first)");

  const str& subject = args[i];
  pcre2_match_data* md = pcre2_match_data_create_from_pattern(state().code, nullptr);
  int rc = pcre2_match(state().code,
                       reinterpret_cast<PCRE2_SPTR>(subject.c_str()), subject.size(),
                       static_cast<PCRE2_SIZE>(offset), 0, md, nullptr);
  if (rc < 0) {
    pcre2_match_data_free(md);
    return 1;  // no match
  }
  PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
  str whole(subject.data() + ov[0], ov[1] - ov[0]);
  (void)ctx.params.set_scalar("PCRE_MATCH", whole);
  if (not var_name.empty())
    (void)ctx.params.set_scalar(var_name, whole);

  vec<str> captures;
  for (int k = 1; k < rc; ++k) {
    PCRE2_SIZE start = ov[2 * k];
    PCRE2_SIZE end   = ov[2 * k + 1];
    if (start == PCRE2_UNSET) captures.emplace_back();
    else captures.emplace_back(subject.data() + start, end - start);
  }
  (void)ctx.params.set_array("pcre_match", captures, false);
  if (not arr_name.empty())
    (void)ctx.params.set_array(arr_name, captures, false);
  pcre2_match_data_free(md);
  return 0;
}

#else   // JFSH_HAVE_PCRE2

int builtin_pcre_compile(vec<str>&, ExecContext&) {
  return err("pcre_compile", "not built in (rebuild with libpcre2-dev)");
}
int builtin_pcre_study(vec<str>&, ExecContext&) {
  return err("pcre_study", "not built in (rebuild with libpcre2-dev)");
}
int builtin_pcre_match(vec<str>&, ExecContext&) {
  return err("pcre_match", "not built in (rebuild with libpcre2-dev)");
}

#endif  // JFSH_HAVE_PCRE2

}  // namespace

void register_pcre_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/pcre", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"pcre_compile", builtin_pcre_compile, BINF_NONE, 1});
    out.builtins.push_back(BuiltinDef{"pcre_study",   builtin_pcre_study,   BINF_NONE, 0});
    out.builtins.push_back(BuiltinDef{"pcre_match",   builtin_pcre_match,   BINF_NONE, 1});
    out.params.emplace_back("PCRE_MATCH");
    out.params.emplace_back("pcre_match");
  });
}

}  // namespace jfsh
