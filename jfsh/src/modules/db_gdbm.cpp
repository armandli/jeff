#include <jfsh/modules/db_gdbm.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

#include <map>
#include <memory>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

#if defined(JFSH_HAVE_GDBM)
#include <gdbm.h>
#elif defined(JFSH_HAVE_NDBM)
#include <ndbm.h>
#endif

// zsh/db/gdbm (M11): ztie / zuntie / zgdbmpath, as zsh's
// Src/Modules/db_gdbm.c.  A tied parameter's every read and write goes to
// the database.  GDBM when it was found at build time; otherwise the
// system's ndbm (macOS libc), whose files aren't GDBM's but which gives
// scripts the same interface.

namespace jfsh {

namespace {

int err(strv cmd, strv msg) { return mod_error(cmd, msg); }

#if defined(JFSH_HAVE_GDBM) || defined(JFSH_HAVE_NDBM)

// One open database, behind the operations the parameter needs.
struct Db {
  str path;
  bool readonly = false;
#ifdef JFSH_HAVE_GDBM
  GDBM_FILE h = nullptr;

  bool open(const str& file, bool ro) {
    h = ::gdbm_open(const_cast<char*>(file.c_str()), 0, ro ? GDBM_READER : GDBM_WRCREAT, 0666, nullptr);
    return h != nullptr;
  }
  void close() { if (h) ::gdbm_close(h); h = nullptr; }
  static datum key_of(const str& k) {
    return datum{const_cast<char*>(k.data()), static_cast<int>(k.size())};
  }
  opt<str> fetch(const str& k) {
    datum d = ::gdbm_fetch(h, key_of(k));
    if (not d.dptr) return s::nullopt;
    str v(d.dptr, static_cast<s::size_t>(d.dsize));
    ::free(d.dptr);
    return v;
  }
  void store(const str& k, const str& v) {
    ::gdbm_store(h, key_of(k), datum{const_cast<char*>(v.data()), static_cast<int>(v.size())}, GDBM_REPLACE);
  }
  void remove(const str& k) { ::gdbm_delete(h, key_of(k)); }
  vec<str> keys() {
    vec<str> out;
    for (datum k = ::gdbm_firstkey(h); k.dptr;) {
      out.emplace_back(k.dptr, static_cast<s::size_t>(k.dsize));
      datum next = ::gdbm_nextkey(h, k);
      ::free(k.dptr);
      k = next;
    }
    return out;
  }
#else
  DBM* h = nullptr;

  bool open(const str& file, bool ro) {
    h = ::dbm_open(file.c_str(), ro ? O_RDONLY : O_RDWR | O_CREAT, 0666);
    return h != nullptr;
  }
  void close() { if (h) ::dbm_close(h); h = nullptr; }
  static datum key_of(const str& k) {
    datum d;
    d.dptr  = const_cast<char*>(k.data());
    d.dsize = k.size();
    return d;
  }
  opt<str> fetch(const str& k) {
    datum d = ::dbm_fetch(h, key_of(k));
    if (not d.dptr) return s::nullopt;
    return str(static_cast<const char*>(d.dptr), static_cast<s::size_t>(d.dsize));
  }
  void store(const str& k, const str& v) {
    datum d;
    d.dptr  = const_cast<char*>(v.data());
    d.dsize = v.size();
    ::dbm_store(h, key_of(k), d, DBM_REPLACE);
  }
  void remove(const str& k) { ::dbm_delete(h, key_of(k)); }
  vec<str> keys() {
    vec<str> out;
    for (datum k = ::dbm_firstkey(h); k.dptr; k = ::dbm_nextkey(h))
      out.emplace_back(static_cast<const char*>(k.dptr), static_cast<s::size_t>(k.dsize));
    return out;
  }
#endif
};

// Tied parameter name → its database.
s::map<str, sptr<Db>>& tied() {
  static s::map<str, sptr<Db>> m;
  return m;
}

int builtin_ztie(vec<str>& args, ExecContext& ctx) {
  str type, file;
  bool ro = false;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-d" and i + 1 < args.size()) type = args[++i];
    else if (a == "-f" and i + 1 < args.size()) file = args[++i];
    else if (a == "-r") ro = true;
    else if (a == "--") { ++i; break; }
    else if (a.size() > 1 and a[0] == '-') return err("ztie", "bad option: " + a);
    else break;
  }
  if (type != "db/gdbm") return err("ztie", "unsupported database type: " + type);
  if (file.empty()) return err("ztie", "database file name required (-f)");
  if (i + 1 != args.size()) return err("ztie", "one parameter name required");
  const str& name = args[i];
  if (tied().count(name)) return err("ztie", "parameter " + name + " is already tied");
  if (auto p = ctx.params.lookup(name); p and p->get().is_readonly())
    return err("ztie", "read-only parameter: " + name);
  auto db = s::make_shared<Db>();
  db->path = file;
  db->readonly = ro;
  if (not db->open(file, ro))
    return err("ztie", "error opening database file " + file + " (" + s::strerror(errno) + ")");
  (void)ctx.params.unset(name);
  Param& p = ctx.params.assign(name, umap<str, str>{},
                               PM_HASH | PM_SPECIAL | (ro ? +PM_READONLY : 0));
  p.handlers = s::make_unique<ParamHandlers>();
  p.handlers->get_hash = [db] {
    umap<str, str> h;
    for (auto& k : db->keys())
      if (auto v = db->fetch(k)) h[k] = s::move(*v);
    return h;
  };
  p.handlers->get_elem = [db](const str& k) { return db->fetch(k); };
  if (not ro) {
    p.handlers->set_elem   = [db](const str& k, str v) { db->store(k, v); };
    p.handlers->unset_elem = [db](const str& k) { db->remove(k); };
    // `name=(k v …)` replaces the whole database.
    p.handlers->set_array = [db](vec<str> words) {
      for (auto& k : db->keys()) db->remove(k);
      for (s::size_t k = 0; k + 1 < words.size(); k += 2) db->store(words[k], words[k + 1]);
    };
  }
  tied()[name] = db;
  return 0;
}

int builtin_zuntie(vec<str>& args, ExecContext& ctx) {
  bool force = false;
  s::size_t i = 1;
  if (i < args.size() and args[i] == "-u") { force = true; ++i; }
  int rc = 0;
  for (; i < args.size(); ++i) {
    auto it = tied().find(args[i]);
    if (it == tied().end()) { rc = err("zuntie", "cannot untie " + args[i]); continue; }
    if (it->second->readonly and not force) {
      rc = err("zuntie", "cannot untie " + args[i] + ": read-only (use -u)");
      continue;
    }
    it->second->close();
    tied().erase(it);
    drop_module_param(ctx.params, args[i]);
  }
  return rc;
}

int builtin_zgdbmpath(vec<str>& args, ExecContext& ctx) {
  if (args.size() != 2) return err("zgdbmpath", "one parameter name required");
  auto it = tied().find(args[1]);
  if (it == tied().end()) return err("zgdbmpath", "no such tied parameter: " + args[1]);
  (void)ctx.params.set_scalar("REPLY", it->second->path);
  return 0;
}

#else  // neither GDBM nor ndbm

int unavailable(strv cmd) { return err(cmd, "database support not available"); }
int builtin_ztie     (vec<str>&, ExecContext&) { return unavailable("ztie"); }
int builtin_zuntie   (vec<str>&, ExecContext&) { return unavailable("zuntie"); }
int builtin_zgdbmpath(vec<str>&, ExecContext&) { return unavailable("zgdbmpath"); }

#endif

}  // namespace

void register_db_gdbm_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/db/gdbm", [](ModuleFeatures& out, ExecContext& ctx) {
    out.builtins.push_back(BuiltinDef{"ztie",      builtin_ztie,      BINF_NONE, 0});
    out.builtins.push_back(BuiltinDef{"zuntie",    builtin_zuntie,    BINF_NONE, 0});
    out.builtins.push_back(BuiltinDef{"zgdbmpath", builtin_zgdbmpath, BINF_NONE, 0});
#if defined(JFSH_HAVE_GDBM) || defined(JFSH_HAVE_NDBM)
    ExecContext* ctxp = &ctx;
    out.on_unload = [ctxp] {
      for (auto& [name, db] : tied()) {
        db->close();
        drop_module_param(ctxp->params, name);
      }
      tied().clear();
    };
#else
    (void)ctx;
#endif
  });
}

}  // namespace jfsh
