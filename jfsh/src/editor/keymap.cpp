#include <jfsh/editor/editor.hh>
namespace jfsh {
std::string_view Keymap::lookup(std::string_view key_seq) const noexcept {
    for (const auto& b : bindings)
        if (b.key_seq == key_seq) return b.widget;
    return {};
}
} // namespace jfsh
