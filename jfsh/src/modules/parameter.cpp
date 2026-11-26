// zsh/parameter: a port of zsh 5.9's Src/Modules/parameter.c.  zsh's
// copyright and licence:
//
// Copyright (c) 1999 Sven Wischnowsky
// All rights reserved.
//
// Permission is hereby granted, without written agreement and without
// license or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall Sven Wischnowsky or the Zsh Development Group be liable
// to any party for direct, indirect, special, incidental, or consequential
// damages arising out of the use of this software and its documentation,
// even if Sven Wischnowsky and the Zsh Development Group have been advised of
// the possibility of such damage.
//
// Sven Wischnowsky and the Zsh Development Group specifically disclaim any
// warranties, including, but not limited to, the implied warranties of
// merchantability and fitness for a particular purpose.  The software
// provided hereunder is on an "as is" basis, and Sven Wischnowsky and the
// Zsh Development Group have no obligation to provide maintenance,
// support, updates, enhancements, or modifications.

#include <jfsh/modules/parameter.h>

#include <grp.h>
#include <pwd.h>
#include <sys/wait.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/core/signals.h>
#include <jfsh/exec/executor.h>
#include <jfsh/history/history.h>
#include <jfsh/parser/parser.h>

// Each parameter is a special whose value is made when it is read, from the
// shell's own tables, so it is always the tables' state.  Those zsh lets
// you assign ($functions, $aliases, $commands, $options, $dirstack …) change
// the tables.  jfsh has no disabled aliases, reserved words or pattern
// characters, nor named directories: $dis_aliases, $dis_galiases,
// $dis_saliases, $dis_reswords, $dis_patchars and $nameddirs are empty.

namespace jfsh {

namespace {

using Hash = umap<str, str>;

// zsh's SPECIALPMDEF flags, and PM_READONLY_SPECIAL.
constexpr s::uint32_t SPECIAL  = PM_SPECIAL | PM_HIDE | PM_HIDEVAL;
constexpr s::uint32_t READONLY = PM_READONLY | PM_RO_BY_DESIGN;   // PM_READONLY_SPECIAL

struct HashHandlers {
  func<Hash()>                         get;
  func<opt<str>(const str&)>           elem;
  func<void(const str&, str)>          set;
  func<void(const str&)>               unset;
  func<void(Hash)>                     set_all;
};

void hash_special(ExecContext& ctx, ModuleFeatures& out, str name, s::uint32_t flags,
                  HashHandlers h) {
  Param& p = ctx.params.assign(name, Hash{}, PM_HASH | SPECIAL | flags);
  p.handlers = s::make_unique<ParamHandlers>();
  p.handlers->get_hash   = s::move(h.get);
  p.handlers->get_elem   = s::move(h.elem);
  p.handlers->set_elem   = s::move(h.set);
  p.handlers->unset_elem = s::move(h.unset);
  p.handlers->set_hash   = s::move(h.set_all);
  out.params.push_back(s::move(name));
}

void array_special(ExecContext& ctx, ModuleFeatures& out, str name, s::uint32_t flags,
                   func<vec<str>()> get, func<void(vec<str>)> set = {}) {
  Param& p = ctx.params.assign(name, vec<str>{}, PM_ARRAY | SPECIAL | flags);
  p.handlers = s::make_unique<ParamHandlers>();
  p.handlers->get_array = s::move(get);
  p.handlers->set_array = s::move(set);
  out.params.push_back(s::move(name));
}

// zsh's hasher, for the order zsh's hash tables give their entries in.
unsigned zsh_hash(strv s) {
  unsigned v = 0;
  for (char c : s) v += (v << 5) + static_cast<unsigned char>(c);
  return v;
}

// zsh's reswds, in the order of its reserved word table (23 buckets, each
// newest first).
vec<str> reswords() {
  static const vec<str> words = [] {
    constexpr const char* RESWDS[] = {
      "!", "[[", "{", "}", "case", "coproc", "declare", "do", "done", "elif", "else", "end",
      "esac", "export", "fi", "float", "for", "foreach", "function", "if", "integer", "local",
      "nocorrect", "readonly", "repeat", "select", "then", "time", "typeset", "until", "while",
    };
    vec<vec<str>> buckets(23);
    for (const char* w : RESWDS) {
      auto& b = buckets[zsh_hash(w) % 23];
      b.insert(b.begin(), w);
    }
    vec<str> out;
    for (auto& b : buckets) out.insert(out.end(), b.begin(), b.end());
    return out;
  }();
  return words;
}

// A function's text as $functions gives it (zsh's getpermtext of the body
// with a tab in front): the definition `functions` prints, without its
// first line and closing brace; with redirections, in braces.
str function_value(const str& name, const CompiledProgram& body) {
  str t = function_text(name, body);
  auto nl = t.find('\n');
  if (nl == str::npos) return "\t";
  str rest = t.substr(nl + 1);
  if (rest.ends_with("\n}")) return rest.substr(0, rest.size() - 2);
  if (rest == "}") return {};
  return "{\n" + rest;
}

// The specials the expander answers itself, not in the parameter table.
constexpr s::pair<const char*, const char*> EXPANDER_SPECIALS[] = {
  {"!", "integer-readonly-special"}, {"?", "integer-readonly-special"},
  {"#", "integer-readonly-special"}, {"$", "integer-readonly-special"},
  {"ARGC", "integer-readonly-special"}, {"@", "array-readonly-special"},
  {"*", "array-readonly-special"}, {"-", "scalar-readonly-special"}, {"0", "scalar-special"},
};

// zsh's paramtypestr.
str type_text(const Param& p) {
  if (not p.is_set()) return {};
  if (p.flags & PM_AUTOLOAD) return "undefined";
  return param_type_text(p);
}

// zsh's pmjobstate.
str job_state(const ExecContext& ctx, const Job& j) {
  const char* cp = j.job_id == ctx.jobs.current()    ? ":+"
                 : j.job_id == ctx.jobs.previous() ? ":-"
                                                   : ":";
  str ret = j.is_done() ? "done" : j.is_stopped() ? "suspended" : "running";
  ret += cp;
  for (const auto& pn : j.procs) {
    str state;
    if (pn.status == ProcStatus::Running) {
      state = "running";
    } else if (pn.status == ProcStatus::Done) {
      // zsh prints the whole wait status.
      state = pn.exit_code ? "exit " + s::to_string(pn.exit_code << 8) : "done";
    } else if (pn.status == ProcStatus::Stopped) {
      state = SignalTable::sig_message(pn.stop_sig);
    } else if (pn.core) {
      state = str(SignalTable::sig_message(pn.stop_sig)) + " (core dumped)";
    } else {
      state = SignalTable::sig_message(pn.stop_sig);
    }
    ret += ":" + s::to_string(pn.pid) + "=" + state;
  }
  return ret;
}

// zsh's pmjobtext.
str job_text(const Job& j) {
  str ret;
  for (s::size_t i = 0; i < j.procs.size(); ++i) {
    if (i) ret += " | ";
    ret += j.procs[i].text;
  }
  return ret;
}

// The job a $jobtexts key names: a number, else a job spec (zsh's getjob).
opt<int> job_of(const ExecContext& ctx, const str& key) {
  char* end = nullptr;
  int job = static_cast<int>(s::strtod(key.c_str(), &end));
  if (*end) {
    auto r = ctx.jobs.find(key);
    if (not r) {
      s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      return s::nullopt;
    }
    job = *r;
  }
  auto j = ctx.jobs.get(job);
  if (job < 1 or not j or j->get().procs.empty()) return s::nullopt;
  return job;
}

// The jobs $jobtexts and the others list: key → value.
Hash jobs_hash(const ExecContext& ctx, func<str(const Job&)> value) {
  Hash h;
  for (int id : ctx.jobs.ids())
    if (auto j = ctx.jobs.get(id); j and not j->get().procs.empty())
      h[s::to_string(id)] = value(j->get());
  return h;
}

// Defines a function from the text given ($functions[name]=text, zsh's
// setfunction).
void set_function(ExecContext& ctx, const str& name, const str& text, bool disabled) {
  auto body = ctx.executor->parse_string(text);
  if (not body) ctx.executor->report_parse_error(body.error());
  if (not body or (*body)->empty()) {
    s::fprintf(stderr, "jfsh: invalid function definition\n");
    return;
  }
  ctx.autoloads.erase(name);
  ctx.functions[name] = *body;
  if (disabled) ctx.disabled_functions.insert(name);
  else          ctx.disabled_functions.erase(name);
}

// zsh's setpmoption.
void set_option(ExecContext& ctx, const str& name, const str& value) {
  bool negated = false;
  auto info = OptionState::find(name, &negated);
  if (value != "on" and value != "off") {
    s::fprintf(stderr, "jfsh: invalid value: %s\n", value.c_str());
  } else if (not info) {
    s::fprintf(stderr, "jfsh: no such option: %s\n", name.c_str());
  } else {
    bool on = (value == "on") != negated;
    OptIndex i = info->get().index;
    if (i == OPT_INTERACTIVE or i == OPT_SHINSTDIN or i == OPT_SINGLECOMMAND
        or ((i == OPT_MONITOR or i == OPT_ZLE) and not ctx.opts.interactive()))
      s::fprintf(stderr, "jfsh: can't change option: %s\n", name.c_str());
    else
      ctx.opts.assign(i, on);
  }
}

opt<str> option_value(const ExecContext& ctx, const str& name) {
  bool negated = false;
  auto info = OptionState::find(name, &negated);
  if (not info) return s::nullopt;
  return ctx.opts.get(info->get().index) != negated ? "on" : "off";
}

// Aliases of one kind ($aliases, $galiases): name → text.
Hash aliases_of(const ExecContext& ctx, bool global) {
  Hash h;
  for (const auto& [name, def] : ctx.aliases.regular)
    if (def.global == global) h[name] = def.text;
  return h;
}

HashHandlers alias_handlers(ExecContext& c, bool global) {
  ExecContext* ctx = &c;
  return {
    [ctx, global] { return aliases_of(*ctx, global); },
    [ctx, global](const str& k) -> opt<str> {
      auto it = ctx->aliases.regular.find(k);
      if (it == ctx->aliases.regular.end() or it->second.global != global) return s::nullopt;
      return it->second.text;
    },
    [ctx, global](const str& k, str v) { ctx->aliases.regular[k] = AliasDef{s::move(v), global}; },
    [ctx](const str& k) { ctx->aliases.regular.erase(k); },
    // zsh's setaliases: the aliases of this kind go first, unless nothing
    // is assigned (`aliases=()` changes nothing).
    [ctx, global](Hash h) {
      if (h.empty()) return;
      s::erase_if(ctx->aliases.regular,
                  [global](const auto& e) { return e.second.global == global; });
      for (auto& [k, v] : h) ctx->aliases.regular[k] = AliasDef{s::move(v), global};
    },
  };
}

// A table jfsh doesn't have ($dis_aliases, $nameddirs): always empty, and
// what is assigned goes nowhere.
HashHandlers empty_handlers() {
  return {[] { return Hash{}; }, [](const str&) -> opt<str> { return s::nullopt; },
          [](const str&, str) {}, [](const str&) {}, [](Hash) {}};
}

// $builtins and $dis_builtins: "defined", or "undefined" for a builtin of
// a module not loaded yet.
HashHandlers builtin_handlers(ExecContext& c, bool disabled) {
  ExecContext* ctx = &c;
  auto value = [ctx, disabled](const str& name) -> opt<str> {
    if (ctx->disabled_builtins.contains(name) != disabled) return s::nullopt;
    if (ctx->modules.autoload_for_builtin(name)) return "undefined";
    if (ctx->executor->is_builtin(name)) return "defined";
    return s::nullopt;
  };
  return {
    [ctx, value] {
      Hash h;
      for (const auto& n : ctx->executor->builtin_names())
        if (auto v = value(n)) h[n] = *v;
      return h;
    },
    value, {}, {}, {},
  };
}

// $functions and $dis_functions.
HashHandlers function_handlers(ExecContext& c, bool disabled) {
  ExecContext* ctx = &c;
  auto value = [ctx, disabled](const str& name) -> opt<str> {
    if (ctx->disabled_functions.contains(name) != disabled) return s::nullopt;
    if (auto it = ctx->functions.find(name); it != ctx->functions.end() and it->second)
      return function_value(name, *it->second);
    if (ctx->autoloads.contains(name)) return autoload_stub_body(*ctx, name, false);
    return s::nullopt;
  };
  return {
    [ctx, value] {
      Hash h;
      for (const auto& [name, body] : ctx->functions)
        if (auto v = value(name)) h[name] = *v;
      for (const auto& [name, flags] : ctx->autoloads)
        if (not h.contains(name))
          if (auto v = value(name)) h[name] = *v;
      return h;
    },
    value,
    [ctx, disabled](const str& k, str v) { set_function(*ctx, k, v, disabled); },
    [ctx](const str& k) {
      ctx->functions.erase(k);
      ctx->autoloads.erase(k);
      ctx->function_files.erase(k);
    },
    [ctx, disabled](Hash h) {
      for (const auto& [k, v] : h) set_function(*ctx, k, v, disabled);
    },
  };
}

// $functions_source and $dis_functions_source: the file a function was
// defined in or loaded from.
HashHandlers function_source_handlers(ExecContext& c, bool disabled) {
  ExecContext* ctx = &c;
  auto value = [ctx, disabled](const str& name) -> opt<str> {
    if (ctx->disabled_functions.contains(name) != disabled) return s::nullopt;
    if (not ctx->functions.contains(name) and not ctx->autoloads.contains(name)) return s::nullopt;
    auto f = ctx->function_files.find(name);
    return f == ctx->function_files.end() ? str{} : f->second;
  };
  return {
    [ctx, value] {
      Hash h;
      for (const auto& [name, body] : ctx->functions)
        if (auto v = value(name)) h[name] = *v;
      for (const auto& [name, flags] : ctx->autoloads)
        if (auto v = value(name)) h[name] = *v;
      return h;
    },
    // zsh's getfunction_source makes an element for any name, empty for
    // one that isn't a function.
    [value](const str& name) -> opt<str> { return value(name).value_or(str{}); },
    {}, {}, {},
  };
}

}  // namespace

void register_parameter_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/parameter", [](ModuleFeatures& out, ExecContext& ctx) {
    ExecContext* c = &ctx;

    hash_special(ctx, out, "aliases", 0, alias_handlers(ctx, false));
    hash_special(ctx, out, "builtins", READONLY, builtin_handlers(ctx, false));

    // $commands: the command hash, every command in $PATH in it (zsh's
    // HASH_LIST_ALL); assigning an element hashes a command.
    hash_special(ctx, out, "commands", 0, {
      [c] { return Hash(c->executor->full_command_hash().begin(),
                        c->executor->full_command_hash().end()); },
      [c](const str& k) -> opt<str> {
        auto& h = c->executor->command_hash();
        if (auto it = h.find(k); it != h.end()) return it->second;
        auto& all = c->executor->full_command_hash();
        if (auto it = all.find(k); it != all.end()) return it->second;
        return s::nullopt;
      },
      [c](const str& k, str v) { c->executor->command_hash()[k] = s::move(v); },
      [c](const str& k) { c->executor->command_hash().erase(k); },
      // zsh's setpmcommands hashes each command; what was hashed stays.
      [c](Hash h) {
        for (auto& [k, v] : h) c->executor->command_hash()[k] = s::move(v);
      },
    });

    array_special(ctx, out, "dirstack", 0, [c] { return c->dirstack; },
                  [c](vec<str> v) { c->dirstack = s::move(v); });
    hash_special(ctx, out, "dis_aliases", 0, empty_handlers());
    hash_special(ctx, out, "dis_builtins", READONLY, builtin_handlers(ctx, true));
    hash_special(ctx, out, "dis_functions", 0, function_handlers(ctx, true));
    hash_special(ctx, out, "dis_functions_source", READONLY, function_source_handlers(ctx, true));
    hash_special(ctx, out, "dis_galiases", 0, empty_handlers());
    array_special(ctx, out, "dis_patchars", READONLY, [] { return vec<str>{}; });
    array_special(ctx, out, "dis_reswords", READONLY, [] { return vec<str>{}; });
    hash_special(ctx, out, "dis_saliases", 0, empty_handlers());

    // The function stack, innermost first.
    auto frames = [c](func<str(s::size_t)> each) {
      vec<str> v;
      for (s::size_t i = c->funcstack.size(); i-- > 0;) v.push_back(each(i));
      return v;
    };
    array_special(ctx, out, "funcfiletrace", READONLY, [c, frames] {
      return frames([c](s::size_t i) {
        const auto& f = c->funcstack[i];
        if (i == 0 or c->funcstack[i - 1].kind == ExecContext::Frame::Source) return f.caller;
        const auto& prev = c->funcstack[i - 1];
        s::uint32_t line = prev.fline + f.line - (prev.kind == ExecContext::Frame::Eval ? 1 : 0);
        return prev.file + ":" + s::to_string(line);
      });
    });
    array_special(ctx, out, "funcsourcetrace", READONLY, [c, frames] {
      return frames([c](s::size_t i) {
        return c->funcstack[i].file + ":" + s::to_string(c->funcstack[i].fline);
      });
    });
    array_special(ctx, out, "funcstack", READONLY, [c, frames] {
      return frames([c](s::size_t i) { return c->funcstack[i].name; });
    });
    hash_special(ctx, out, "functions", 0, function_handlers(ctx, false));
    hash_special(ctx, out, "functions_source", READONLY, function_source_handlers(ctx, false));
    array_special(ctx, out, "functrace", READONLY, [c, frames] {
      return frames([c](s::size_t i) { return c->funcstack[i].caller; });
    });
    hash_special(ctx, out, "galiases", 0, alias_handlers(ctx, true));

    // $history: event number → line; $historywords: the words, newest
    // first.
    hash_special(ctx, out, "history", READONLY, {
      [c] {
        Hash h;
        if (c->history)
          for (const auto& e : c->history->entries()) h[s::to_string(e.num)] = e.text;
        return h;
      },
      [c](const str& k) -> opt<str> {
        if (not c->history or k.empty() or k.find_first_not_of("0123456789") != str::npos
            or (k[0] == '0' and k.size() > 1))
          return s::nullopt;
        auto n = s::strtoul(k.c_str(), nullptr, 10);
        for (const auto& e : c->history->entries())
          if (e.num == n) return e.text;
        return s::nullopt;
      },
      {}, {}, {},
    });
    array_special(ctx, out, "historywords", READONLY, [c] {
      vec<str> v;
      if (not c->history) return v;
      const auto& all = c->history->entries();
      for (auto it = all.rbegin(); it != all.rend(); ++it) {
        auto words = HistoryManager::split_words(it->text);
        v.insert(v.end(), words.rbegin(), words.rend());
      }
      return v;
    });

    // The jobs: the directory each was started in (the current one, as
    // jfsh keeps no other), its state and its text.
    auto job_handlers = [c](func<str(const Job&)> value) -> HashHandlers {
      return {
        [c, value] { return jobs_hash(*c, value); },
        [c, value](const str& k) -> opt<str> {
          auto job = job_of(*c, k);
          if (not job) return s::nullopt;
          return value(c->jobs.get(*job)->get());
        },
        {}, {}, {},
      };
    };
    hash_special(ctx, out, "jobdirs", READONLY, job_handlers([c](const Job&) {
      auto p = c->params.lookup("PWD");
      return p ? p->get().scalar() : str{};
    }));
    hash_special(ctx, out, "jobstates", READONLY,
                 job_handlers([c](const Job& j) { return job_state(*c, j); }));
    hash_special(ctx, out, "jobtexts", READONLY,
                 job_handlers([](const Job& j) { return job_text(j); }));

    hash_special(ctx, out, "modules", READONLY, {
      [c] {
        Hash h;
        for (auto& [name, state] : c->modules.module_states()) h[name] = state;
        return h;
      },
      [c](const str& k) -> opt<str> {
        for (auto& [name, state] : c->modules.module_states())
          if (name == k) return state;
        return s::nullopt;
      },
      {}, {}, {},
    });
    hash_special(ctx, out, "nameddirs", 0, empty_handlers());

    // $options: each option (and option alias) on or off; assigning sets
    // the option.
    hash_special(ctx, out, "options", 0, {
      [c] {
        Hash h;
        for (const auto& info : OPTION_INFO) h[info.name] = c->opts.get(info.index) ? "on" : "off";
        for (strv alias : option_alias_names()) h[str(alias)] = *option_value(*c, str(alias));
        return h;
      },
      [c](const str& k) { return option_value(*c, k); },
      [c](const str& k, str v) { set_option(*c, k, v); },
      [c](const str& k) { set_option(*c, k, "off"); },
      [c](Hash h) {
        for (const auto& [k, v] : h) set_option(*c, k, v);
      },
    });

    // $parameters: each set parameter's type.  The values aren't made: the
    // type is in the flags.
    hash_special(ctx, out, "parameters", READONLY, {
      [c] {
        Hash h;
        c->params.for_each({}, [&](const Param& p) {
          if (p.is_set()) h[p.name] = type_text(p);
          return false;
        });
        for (auto [name, type] : EXPANDER_SPECIALS) h.try_emplace(name, type);
        return h;
      },
      [c](const str& k) -> opt<str> {
        auto p = c->params.lookup_noget(k);
        if (not p or not p->get().is_set()) {
          for (auto [name, type] : EXPANDER_SPECIALS)
            if (k == name) return str(type);
          return s::nullopt;
        }
        return type_text(p->get());
      },
      {}, {}, {},
    });
    array_special(ctx, out, "patchars", READONLY, [] {
      return vec<str>{"|", "~", "(", "?", "*", "[", "<", "^", "#",
                      "?(", "*(", "+(", "!(", "\\!(", "@("};
    });
    array_special(ctx, out, "reswords", READONLY, [] { return reswords(); });
    hash_special(ctx, out, "saliases", 0, {
      [c] { return Hash(c->aliases.suffix.begin(), c->aliases.suffix.end()); },
      [c](const str& k) -> opt<str> {
        auto it = c->aliases.suffix.find(k);
        if (it == c->aliases.suffix.end()) return s::nullopt;
        return it->second;
      },
      [c](const str& k, str v) { c->aliases.suffix[k] = s::move(v); },
      [c](const str& k) { c->aliases.suffix.erase(k); },
      [c](Hash h) {
        if (h.empty()) return;
        c->aliases.suffix.clear();
        for (auto& [k, v] : h) c->aliases.suffix[k] = s::move(v);
      },
    });

    // $userdirs: each user's home directory; $usergroups: each group's id.
    hash_special(ctx, out, "userdirs", READONLY, {
      [] {
        Hash h;
        ::setpwent();
        while (struct passwd* pw = ::getpwent())
          if (pw->pw_name and pw->pw_dir) h.emplace(pw->pw_name, pw->pw_dir);
        ::endpwent();
        return h;
      },
      [](const str& k) -> opt<str> {
        if (struct passwd* pw = ::getpwnam(k.c_str()); pw and pw->pw_dir) return str(pw->pw_dir);
        return s::nullopt;
      },
      {}, {}, {},
    });
    hash_special(ctx, out, "usergroups", READONLY, {
      [] {
        Hash h;
        ::setgrent();
        while (struct group* gr = ::getgrent())
          if (gr->gr_name) h.emplace(gr->gr_name, s::to_string(gr->gr_gid));
        ::endgrent();
        return h;
      },
      [](const str& k) -> opt<str> {
        if (struct group* gr = ::getgrnam(k.c_str())) return s::to_string(gr->gr_gid);
        return s::nullopt;
      },
      {}, {}, {},
    });
  });
}

}  // namespace jfsh
