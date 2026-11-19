#include <jfsh/modules/deltochar.h>

#include <jfsh/core/aliases.h>
#include <jfsh/editor/editor.h>
#include <jfsh/editor/zle.h>
#include <jfsh/exec/executor.h>

// zsh/deltochar (M11): the delete-to-char and zap-to-char widgets, as
// zsh's Src/Zle/deltochar.c.  They live in Zle::module_widgets while the
// module is loaded.

namespace jfsh {

namespace {

// Reads a character, finds its |n|th occurrence after (n > 0) or before
// (n < 0) the cursor, and kills up to it — the character included when
// `inclusive`.  Fails, leaving the line alone, when there aren't n.
bool kill_to_char(Editor& ed, bool inclusive) {
  str ch = ed.read_char();
  if (ch.empty()) return false;
  int n = ed.repeat_count();
  LineBuffer& buf = ed.buffer();
  const str& text = buf.text;
  s::size_t cur = buf.cursor;
  if (n > 0) {
    s::size_t pos = cur;
    for (int k = 0; k < n; ++k) {
      // As zsh: the character under the cursor counts as the first one.
      pos = text.find(ch, k == 0 ? pos : pos + ch.size());
      if (pos == str::npos) return false;
    }
    ed.kill_range(cur, inclusive ? pos + ch.size() : pos, false);
    buf.cursor = cur;
    return true;
  }
  if (n == 0 or cur == 0) return false;
  s::size_t pos = cur;
  for (int k = 0; k < -n; ++k) {
    if (pos == 0) return false;
    pos = text.rfind(ch, pos - 1);
    if (pos == str::npos) return false;
  }
  s::size_t from = inclusive ? pos : pos + ch.size();
  ed.kill_range(from, cur, true);
  buf.cursor = from;
  return true;
}

}  // namespace

void register_deltochar_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/deltochar", [](ModuleFeatures& out, ExecContext& ctx) {
    if (not ctx.zle) return;
    Zle* zle = ctx.zle;
    zle->module_widgets["delete-to-char"] = [](Editor& ed) { return kill_to_char(ed, true); };
    zle->module_widgets["zap-to-char"]    = [](Editor& ed) { return kill_to_char(ed, false); };
    out.on_unload = [zle] {
      zle->module_widgets.erase("delete-to-char");
      zle->module_widgets.erase("zap-to-char");
    };
    out.feature_names = {"z:delete-to-char", "z:zap-to-char"};
  });
}

}  // namespace jfsh
