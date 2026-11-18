#include <jfsh/modules/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

// zsh/stat (M10 Phase 2c): zstat.  Mirrors zsh's Src/Modules/stat.c in
// shape (element list, formatting flags, -A / -H output modes) but shares
// jfsh's error-wording and option-parsing style.

namespace jfsh {

namespace {

int err(strv cmd, strv msg) {
  s::fprintf(stderr, "jfsh: %.*s: %.*s\n",
             static_cast<int>(cmd.size()), cmd.data(),
             static_cast<int>(msg.size()), msg.data());
  return 1;
}

// The 15 element names zsh exposes, in zsh's order.
constexpr const char* ELEMENT_NAMES[] = {
  "device", "inode", "mode", "nlink", "uid", "gid", "rdev",
  "size", "atime", "mtime", "ctime", "blksize", "blocks",
  "link", "type",
};
constexpr s::size_t NUM_ELEMENTS = sizeof ELEMENT_NAMES / sizeof *ELEMENT_NAMES;

// `type` values (zsh's names).
static const char* mode_to_type(mode_t m) {
  if (S_ISREG(m))  return "regular";
  if (S_ISDIR(m))  return "directory";
  if (S_ISLNK(m))  return "link";
  if (S_ISCHR(m))  return "character special";
  if (S_ISBLK(m))  return "block special";
  if (S_ISFIFO(m)) return "fifo";
  if (S_ISSOCK(m)) return "socket";
  return "unknown";
}

struct FormatOpts {
  bool string_mode = false;   // -s: strings for uid/gid/mode/type
  bool raw         = false;   // -r: raw numeric (no symbolic formatting)
  bool octal       = false;   // -o: octal for mode
  bool numeric_ts  = false;   // -t: timestamps as `atime - MM/DD` → integer
  bool follow      = true;    // -L: don't follow (use lstat)
  bool with_name   = true;    // -n/-N: include file name
};

// Render a single element as zsh would.
static str render(const struct stat& st, strv link_target, int element,
                  const FormatOpts& fo) {
  switch (element) {
    case 0:  return s::to_string(static_cast<unsigned long>(st.st_dev));
    case 1:  return s::to_string(static_cast<unsigned long>(st.st_ino));
    case 2: {
      if (fo.octal) {
        char buf[16]; s::snprintf(buf, sizeof buf, "0%o", st.st_mode & 07777);
        return buf;
      }
      if (fo.string_mode) {
        // -rwxr-xr-x style.
        str s(10, '-');
        mode_t m = st.st_mode;
        if (S_ISDIR(m)) s[0] = 'd';
        else if (S_ISLNK(m)) s[0] = 'l';
        else if (S_ISCHR(m)) s[0] = 'c';
        else if (S_ISBLK(m)) s[0] = 'b';
        else if (S_ISFIFO(m)) s[0] = 'p';
        else if (S_ISSOCK(m)) s[0] = 's';
        if (m & S_IRUSR) s[1] = 'r';
        if (m & S_IWUSR) s[2] = 'w';
        if (m & S_IXUSR) s[3] = 'x';
        if (m & S_IRGRP) s[4] = 'r';
        if (m & S_IWGRP) s[5] = 'w';
        if (m & S_IXGRP) s[6] = 'x';
        if (m & S_IROTH) s[7] = 'r';
        if (m & S_IWOTH) s[8] = 'w';
        if (m & S_IXOTH) s[9] = 'x';
        return s;
      }
      return s::to_string(st.st_mode);
    }
    case 3:  return s::to_string(static_cast<unsigned long>(st.st_nlink));
    case 4: {
      if (fo.string_mode) {
        if (struct passwd* pw = ::getpwuid(st.st_uid)) return pw->pw_name;
      }
      return s::to_string(st.st_uid);
    }
    case 5: {
      if (fo.string_mode) {
        if (struct group* g = ::getgrgid(st.st_gid)) return g->gr_name;
      }
      return s::to_string(st.st_gid);
    }
    case 6:  return s::to_string(static_cast<unsigned long>(st.st_rdev));
    case 7:  return s::to_string(static_cast<long long>(st.st_size));
    case 8:
    case 9:
    case 10: {
      time_t t = (element == 8) ? st.st_atime : (element == 9) ? st.st_mtime : st.st_ctime;
      if (fo.numeric_ts or fo.raw) return s::to_string(static_cast<long long>(t));
      struct tm tm{}; ::localtime_r(&t, &tm);
      char buf[64]; s::strftime(buf, sizeof buf, "%a %b %d %H:%M:%S %Y", &tm);
      return buf;
    }
    case 11: return s::to_string(static_cast<long>(st.st_blksize));
    case 12: return s::to_string(static_cast<long long>(st.st_blocks));
    case 13: return str(link_target);
    case 14: return mode_to_type(st.st_mode);
    default: return {};
  }
}

static int element_index(strv name) {
  str n(name);
  if (not n.empty() and n[0] == '+') n.erase(0, 1);
  for (s::size_t i = 0; i < NUM_ELEMENTS; ++i)
    if (n == ELEMENT_NAMES[i]) return static_cast<int>(i);
  return -1;
}

int builtin_zstat(vec<str>& args, ExecContext& ctx) {
  FormatOpts fo;
  str array_name, hash_name;
  bool list_elements = false;
  bool no_name = false;
  vec<int> selected;    // -1 means "all elements"
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (not a.empty() and a[0] == '+') {
      int idx = element_index(a);
      if (idx < 0) return err("zstat", "unknown element: " + a);
      selected.push_back(idx);
      continue;
    }
    if (a.size() < 2 or a[0] != '-' or a == "--") {
      if (a == "--") ++i;
      break;
    }
    if (a == "-A") {
      if (++i >= args.size()) return err("zstat", "option -A requires an argument");
      array_name = args[i];
    } else if (a == "-H") {
      if (++i >= args.size()) return err("zstat", "option -H requires an argument");
      hash_name = args[i];
    } else {
      for (s::size_t k = 1; k < a.size(); ++k) {
        switch (a[k]) {
          case 'l': list_elements = true; break;
          case 'L': fo.follow = false; break;
          case 'n': fo.with_name = true;  no_name = false; break;
          case 'N': fo.with_name = false; no_name = true;  break;
          case 'o': fo.octal = true; break;
          case 'r': fo.raw   = true; break;
          case 's': fo.string_mode = true; break;
          case 't': fo.numeric_ts  = true; break;
          case 'T': fo.numeric_ts  = true; fo.raw = true; break;
          case 'g': /* use glob: already done by the expander */ break;
          default:
            s::fprintf(stderr, "jfsh: zstat: bad option: -%c\n", a[k]);
            return 1;
        }
      }
    }
  }

  if (list_elements) {
    for (s::size_t k = 0; k < NUM_ELEMENTS; ++k)
      s::printf("%s\n", ELEMENT_NAMES[k]);
    return 0;
  }

  // If only one path and multiple elements asked for via +elem, that's
  // still allowed.
  if (i >= args.size()) return err("zstat", "not enough arguments");

  // If no +elements were given, output all 15.
  vec<int> elems = selected.empty()
      ? [] { vec<int> v; v.reserve(NUM_ELEMENTS);
             for (int k = 0; k < static_cast<int>(NUM_ELEMENTS); ++k) v.push_back(k);
             return v; }()
      : selected;

  int rc = 0;
  for (; i < args.size(); ++i) {
    const str& path = args[i];
    struct stat st{};
    int sr = fo.follow ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st);
    if (sr < 0) {
      rc = err("zstat", str(s::strerror(errno)) + ": " + path);
      continue;
    }

    str link;
    if (S_ISLNK(st.st_mode)) {
      char buf[4096];
      ssize_t n = ::readlink(path.c_str(), buf, sizeof buf);
      if (n > 0) link.assign(buf, static_cast<s::size_t>(n));
    }

    if (not hash_name.empty()) {
      vec<str> flat;
      for (int e : elems) {
        flat.push_back(ELEMENT_NAMES[e]);
        flat.push_back(render(st, link, e, fo));
      }
      ctx.params.assign(hash_name, umap<str, str>{}, PM_HASH);
      (void)ctx.params.set_array(hash_name, flat, false);
      continue;
    }
    if (not array_name.empty()) {
      vec<str> flat;
      for (int e : elems) flat.push_back(render(st, link, e, fo));
      (void)ctx.params.set_array(array_name, flat, false);
      continue;
    }

    // stdout mode: one line per element (or just the value when a single
    // element was asked for).
    (void)no_name;
    bool single = (elems.size() == 1);
    for (int e : elems) {
      if (single) {
        s::printf("%s\n", render(st, link, e, fo).c_str());
      } else {
        if (fo.with_name) s::printf("%s ", path.c_str());
        s::printf("%s %s\n", ELEMENT_NAMES[e], render(st, link, e, fo).c_str());
      }
    }
  }
  return rc;
}

}  // namespace

void register_stat_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/stat", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"zstat", builtin_zstat, BINF_NONE, 1});
    out.builtins.push_back(BuiltinDef{"stat",  builtin_zstat, BINF_NONE, 1});
  });
}

}  // namespace jfsh
