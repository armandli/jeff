#include <jfsh/core/aliases.h>
#include <jfsh/editor/editor.h>

namespace jfsh {

strv Keymap::lookup(strv key_seq) const noexcept {
  for (const auto& b : bindings)
    if (b.key_seq == key_seq) return b.widget;
  return {};
}

}  // namespace jfsh
