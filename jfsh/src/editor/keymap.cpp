#include <jfsh/core/aliases.h>
#include <jfsh/editor/editor.h>
#include <jfsh/editor/zle.h>

#include <cctype>
#include <cstdlib>

#include <algorithm>

namespace jfsh {

const KeyBinding* Keymap::lookup(strv seq) const {
  auto it = bindings.find(str(seq));
  return it == bindings.end() ? nullptr : &it->second;
}

bool Keymap::is_prefix(strv seq) const {
  auto it = bindings.upper_bound(str(seq));
  return it != bindings.end() and it->first.size() > seq.size()
     and strv(it->first).starts_with(seq);
}

void Keymap::bind(strv seq, str widget) {
  if (seq.empty()) return;
  bindings[str(seq)] = KeyBinding{s::move(widget), {}, false};
}

void Keymap::bind_macro(strv seq, str keys) {
  if (seq.empty()) return;
  bindings[str(seq)] = KeyBinding{{}, s::move(keys), true};
}

str parse_key_notation(strv t) {
  str out;
  for (s::size_t i = 0; i < t.size(); ++i) {
    char c = t[i];
    bool meta = false;
    if (c == '\\' and i + 2 < t.size() and t[i + 1] == 'M' and t[i + 2] == '-') {
      meta = true;
      i += 3;
      if (i >= t.size()) break;
      c = t[i];
    }
    auto emit = [&](unsigned char b) {
      if (meta) b |= 0x80;
      out += static_cast<char>(b);
    };
    if (c == '\\' and i + 2 < t.size() and t[i + 1] == 'C' and t[i + 2] == '-') {
      i += 3;
      if (i >= t.size()) break;
      unsigned char k = static_cast<unsigned char>(t[i]);
      emit(k == '?' ? 0x7f : static_cast<unsigned char>(s::toupper(k) & 0x1f));
      continue;
    }
    if (c == '^' and i + 1 < t.size()) {
      unsigned char k = static_cast<unsigned char>(t[++i]);
      emit(k == '?' ? 0x7f : static_cast<unsigned char>(s::toupper(k) & 0x1f));
      continue;
    }
    if (c != '\\' or i + 1 >= t.size()) {
      emit(static_cast<unsigned char>(c));
      continue;
    }
    char e = t[++i];
    switch (e) {
      case 'e': case 'E': emit(0x1b); break;
      case 'n': emit('\n'); break;
      case 't': emit('\t'); break;
      case 'r': emit('\r'); break;
      case 'a': emit('\a'); break;
      case 'b': emit('\b'); break;
      case 'f': emit('\f'); break;
      case 'v': emit('\v'); break;
      case 'x': {
        unsigned v = 0;
        int n = 0;
        while (n < 2 and i + 1 < t.size() and s::isxdigit(static_cast<unsigned char>(t[i + 1]))) {
          char h = t[++i];
          v = v * 16 + static_cast<unsigned>(s::isdigit(static_cast<unsigned char>(h)) ? h - '0' : (s::tolower(h) - 'a' + 10));
          ++n;
        }
        emit(static_cast<unsigned char>(v));
        break;
      }
      default:
        if (e >= '0' and e <= '7') {
          unsigned v = static_cast<unsigned>(e - '0');
          int n = 1;
          while (n < 3 and i + 1 < t.size() and t[i + 1] >= '0' and t[i + 1] <= '7') {
            v = v * 8 + static_cast<unsigned>(t[++i] - '0');
            ++n;
          }
          emit(static_cast<unsigned char>(v));
        } else {
          emit(static_cast<unsigned char>(e));
        }
    }
  }
  return out;
}

str format_key(strv seq) {
  str out;
  for (char ch : seq) {
    auto c = static_cast<unsigned char>(ch);
    if (c >= 0x80) {
      out += "\\M-";
      c &= 0x7f;
    }
    if (c < 0x20)       { out += '^'; out += static_cast<char>(c | 0x40); if (c == 0x1c) out += '\\'; }
    else if (c == 0x7f) out += "^?";
    else if (c == '\\' or c == '"' or c == '$' or c == '^' or c == '`') { out += '\\'; out += static_cast<char>(c); }
    else                out += static_cast<char>(c);
  }
  return out;
}

namespace {

void bind_range(Keymap& km, int lo, int hi, strv widget) {
  for (int c = lo; c <= hi; ++c) km.bind(str(1, static_cast<char>(c)), str(widget));
}

void bind_all(Keymap& km, s::initializer_list<s::pair<const char*, const char*>> list) {
  for (auto& [k, w] : list) km.bind(parse_key_notation(k), w);
}

// The keys a terminal sends that every editing keymap understands.
void bind_terminal_keys(Keymap& km, bool vi) {
  bind_all(km, {
    {"^[[A", "up-line-or-history"},   {"^[OA", "up-line-or-history"},
    {"^[[B", "down-line-or-history"}, {"^[OB", "down-line-or-history"},
    {"^[[200~", "bracketed-paste"},
  });
  const char* fwd  = vi ? "vi-forward-char" : "forward-char";
  const char* back = vi ? "vi-backward-char" : "backward-char";
  for (const char* k : {"^[[C", "^[OC"}) km.bind(parse_key_notation(k), fwd);
  for (const char* k : {"^[[D", "^[OD"}) km.bind(parse_key_notation(k), back);
  // Home, End and Delete (zsh leaves these to the user's terminfo setup).
  const char* bol = vi ? "vi-beginning-of-line" : "beginning-of-line";
  const char* eol = vi ? "vi-end-of-line" : "end-of-line";
  for (const char* k : {"^[[H", "^[OH", "^[[1~", "^[[7~"}) km.bind(parse_key_notation(k), bol);
  for (const char* k : {"^[[F", "^[OF", "^[[4~", "^[[8~"}) km.bind(parse_key_notation(k), eol);
  km.bind(parse_key_notation("^[[3~"), vi ? "vi-delete-char" : "delete-char");
  km.bind(parse_key_notation("^[[Z"), "reverse-menu-complete");
}

sptr<Keymap> make_emacs() {
  auto km = s::make_shared<Keymap>();
  km->insert_unbound = true;
  bind_all(*km, {
    {"^@", "set-mark-command"}, {"^A", "beginning-of-line"}, {"^B", "backward-char"},
    {"^D", "delete-char-or-list"}, {"^E", "end-of-line"}, {"^F", "forward-char"},
    {"^G", "send-break"}, {"^H", "backward-delete-char"}, {"^I", "expand-or-complete"},
    {"^J", "accept-line"}, {"^K", "kill-line"}, {"^L", "clear-screen"},
    {"^M", "accept-line"}, {"^N", "down-line-or-history"},
    {"^O", "accept-line-and-down-history"}, {"^P", "up-line-or-history"},
    {"^Q", "push-line"}, {"^R", "history-incremental-search-backward"},
    {"^S", "history-incremental-search-forward"}, {"^T", "transpose-chars"},
    {"^U", "kill-whole-line"}, {"^V", "quoted-insert"}, {"^W", "backward-kill-word"},
    {"^X^B", "vi-match-bracket"}, {"^X^E", "edit-command-line"},
    {"^X^F", "vi-find-next-char"}, {"^X^J", "vi-join"}, {"^X^K", "kill-buffer"},
    {"^X^N", "infer-next-history"}, {"^X^O", "overwrite-mode"}, {"^X^U", "undo"},
    {"^X^V", "vi-cmd-mode"}, {"^X^X", "exchange-point-and-mark"},
    {"^X*", "expand-word"}, {"^X=", "what-cursor-position"}, {"^XG", "list-expand"},
    {"^Xg", "list-expand"}, {"^Xr", "history-incremental-search-backward"},
    {"^Xs", "history-incremental-search-forward"}, {"^Xu", "undo"}, {"^Y", "yank"},
    {"^[^D", "list-choices"}, {"^[^G", "send-break"}, {"^[^H", "backward-kill-word"},
    {"^[^I", "self-insert-unmeta"}, {"^[^J", "self-insert-unmeta"},
    {"^[^L", "clear-screen"}, {"^[^M", "self-insert-unmeta"}, {"^[^_", "copy-prev-word"},
    {"^[ ", "expand-history"}, {"^[!", "expand-history"}, {"^[\"", "quote-region"},
    {"^[\\$", "spell-word"}, {"^['", "quote-line"}, {"^[-", "neg-argument"},
    {"^[.", "insert-last-word"}, {"^[<", "beginning-of-buffer-or-history"},
    {"^[>", "end-of-buffer-or-history"}, {"^[?", "which-command"},
    {"^[A", "accept-and-hold"}, {"^[B", "backward-word"}, {"^[C", "capitalize-word"},
    {"^[D", "kill-word"}, {"^[F", "forward-word"}, {"^[G", "get-line"},
    {"^[H", "run-help"}, {"^[L", "down-case-word"}, {"^[N", "history-search-forward"},
    {"^[P", "history-search-backward"}, {"^[Q", "push-line"}, {"^[S", "spell-word"},
    {"^[T", "transpose-words"}, {"^[U", "up-case-word"}, {"^[W", "copy-region-as-kill"},
    {"^[_", "insert-last-word"}, {"^[a", "accept-and-hold"}, {"^[b", "backward-word"},
    {"^[c", "capitalize-word"}, {"^[d", "kill-word"}, {"^[f", "forward-word"},
    {"^[g", "get-line"}, {"^[h", "run-help"}, {"^[l", "down-case-word"},
    {"^[n", "history-search-forward"}, {"^[p", "history-search-backward"},
    {"^[q", "push-line"}, {"^[s", "spell-word"}, {"^[t", "transpose-words"},
    {"^[u", "up-case-word"}, {"^[w", "copy-region-as-kill"},
    {"^[x", "execute-named-cmd"}, {"^[y", "yank-pop"}, {"^[z", "execute-last-named-cmd"},
    {"^[|", "vi-goto-column"}, {"^[^?", "backward-kill-word"}, {"^_", "undo"},
    {"^?", "backward-delete-char"},
  });
  for (char d = '0'; d <= '9'; ++d) km->bind(str("\x1b") + d, "digit-argument");
  bind_range(*km, ' ', '~', "self-insert");
  bind_terminal_keys(*km, false);
  return km;
}

sptr<Keymap> make_viins() {
  auto km = s::make_shared<Keymap>();
  km->insert_unbound = true;
  bind_range(*km, 0x01, 0x1f, "self-insert");
  bind_range(*km, ' ', '~', "self-insert");
  bind_all(*km, {
    {"^D", "list-choices"}, {"^G", "list-expand"}, {"^H", "vi-backward-delete-char"},
    {"^I", "expand-or-complete"}, {"^J", "accept-line"}, {"^L", "clear-screen"},
    {"^M", "accept-line"}, {"^Q", "vi-quoted-insert"}, {"^R", "redisplay"},
    {"^U", "vi-kill-line"}, {"^V", "vi-quoted-insert"}, {"^W", "vi-backward-kill-word"},
    {"^[", "vi-cmd-mode"}, {"^?", "vi-backward-delete-char"},
  });
  bind_terminal_keys(*km, true);
  return km;
}

sptr<Keymap> make_vicmd() {
  auto km = s::make_shared<Keymap>();
  bind_all(*km, {
    {"^D", "list-choices"}, {"^G", "list-expand"}, {"^H", "vi-backward-char"},
    {"^J", "accept-line"}, {"^L", "clear-screen"}, {"^M", "accept-line"},
    {"^N", "down-history"}, {"^P", "up-history"}, {"^R", "redo"}, {"^[", "beep"},
    {" ", "vi-forward-char"}, {"\"", "vi-set-buffer"}, {"#", "pound-insert"},
    {"$", "vi-end-of-line"}, {"%", "vi-match-bracket"}, {"'", "vi-goto-mark-line"},
    {"+", "vi-down-line-or-history"}, {",", "vi-rev-repeat-find"},
    {"-", "vi-up-line-or-history"}, {".", "vi-repeat-change"},
    {"/", "vi-history-search-backward"}, {"0", "vi-digit-or-beginning-of-line"},
    {":", "execute-named-cmd"}, {";", "vi-repeat-find"}, {"<", "vi-unindent"},
    {"=", "list-choices"}, {">", "vi-indent"}, {"?", "vi-history-search-forward"},
    {"A", "vi-add-eol"}, {"B", "vi-backward-blank-word"}, {"C", "vi-change-eol"},
    {"D", "vi-kill-eol"}, {"E", "vi-forward-blank-word-end"}, {"F", "vi-find-prev-char"},
    {"G", "vi-fetch-history"}, {"I", "vi-insert-bol"}, {"J", "vi-join"},
    {"N", "vi-rev-repeat-search"}, {"O", "vi-open-line-above"}, {"P", "vi-put-before"},
    {"R", "vi-replace"}, {"S", "vi-change-whole-line"}, {"T", "vi-find-prev-char-skip"},
    {"V", "visual-line-mode"}, {"W", "vi-forward-blank-word"},
    {"X", "vi-backward-delete-char"}, {"Y", "vi-yank-whole-line"},
    {"\\^", "vi-first-non-blank"}, {"`", "vi-goto-mark"}, {"a", "vi-add-next"},
    {"b", "vi-backward-word"}, {"c", "vi-change"}, {"d", "vi-delete"},
    {"e", "vi-forward-word-end"}, {"f", "vi-find-next-char"},
    {"gE", "vi-backward-blank-word-end"}, {"gU", "vi-up-case"},
    {"ge", "vi-backward-word-end"}, {"gg", "beginning-of-buffer-or-history"},
    {"gu", "vi-down-case"}, {"g~", "vi-oper-swap-case"},
    {"h", "vi-backward-char"}, {"i", "vi-insert"}, {"j", "down-line-or-history"},
    {"k", "up-line-or-history"}, {"l", "vi-forward-char"}, {"m", "vi-set-mark"},
    {"n", "vi-repeat-search"}, {"o", "vi-open-line-below"}, {"p", "vi-put-after"},
    {"r", "vi-replace-chars"}, {"s", "vi-substitute"}, {"t", "vi-find-next-char-skip"},
    {"u", "undo"}, {"v", "visual-mode"}, {"w", "vi-forward-word"},
    {"x", "vi-delete-char"}, {"y", "vi-yank"}, {"|", "vi-goto-column"},
    {"~", "vi-swap-case"}, {"^?", "vi-backward-char"},
  });
  for (char d = '1'; d <= '9'; ++d) km->bind(str(1, d), "digit-argument");
  km->bind_macro("gUU", "gUgU");
  km->bind_macro("guu", "gugu");
  km->bind_macro("g~~", "g~g~");
  bind_terminal_keys(*km, true);
  return km;
}

void bind_selection(Keymap& km) {
  bind_all(km, {
    {"^[OA", "up-line"}, {"^[OB", "down-line"}, {"^[[A", "up-line"}, {"^[[B", "down-line"},
    {"aW", "select-a-blank-word"}, {"aa", "select-a-shell-word"}, {"aw", "select-a-word"},
    {"iW", "select-in-blank-word"}, {"ia", "select-in-shell-word"}, {"iw", "select-in-word"},
    {"j", "down-line"}, {"k", "up-line"},
  });
  // Quote and bracket objects (zsh has them as the select-quoted and
  // select-bracketed shell functions).
  for (const char* q : {"\"", "'", "`"}) {
    km.bind(str("i") + q, "select-in-quoted");
    km.bind(str("a") + q, "select-a-quoted");
  }
  for (const char* b : {"(", ")", "b", "[", "]", "{", "}", "B", "<", ">"}) {
    km.bind(str("i") + b, "select-in-bracketed");
    km.bind(str("a") + b, "select-a-bracketed");
  }
}

sptr<Keymap> make_visual() {
  auto km = s::make_shared<Keymap>();
  bind_selection(*km);
  bind_all(*km, {
    {"^[", "deactivate-region"}, {"U", "vi-up-case"}, {"o", "exchange-point-and-mark"},
    {"p", "put-replace-selection"}, {"u", "vi-down-case"}, {"x", "vi-delete"},
    {"~", "vi-oper-swap-case"},
  });
  return km;
}

sptr<Keymap> make_viopp() {
  auto km = s::make_shared<Keymap>();
  bind_selection(*km);
  km->bind(parse_key_notation("^["), "vi-cmd-mode");
  return km;
}

sptr<Keymap> make_safe() {
  auto km = s::make_shared<Keymap>();
  km->insert_unbound = true;
  bind_range(*km, ' ', '~', "self-insert");
  km->bind("\n", "accept-line");
  km->bind("\r", "accept-line");
  return km;
}

}  // namespace

Zle::Zle() { reset(); }

void Zle::reset() {
  bool vi = false;
  if (auto m = keymaps.find("main"); m != keymaps.end()) {
    auto v = keymaps.find("viins");
    vi = v != keymaps.end() and m->second == v->second;
  }
  keymaps.clear();
  keymaps["emacs"]   = make_emacs();
  keymaps["viins"]   = make_viins();
  keymaps["vicmd"]   = make_vicmd();
  keymaps["visual"]  = make_visual();
  keymaps["viopp"]   = make_viopp();
  keymaps["isearch"] = s::make_shared<Keymap>();
  keymaps["command"] = s::make_shared<Keymap>();
  keymaps[".safe"]   = make_safe();
  select_main(vi);
}

sptr<Keymap> Zle::keymap(strv name) const {
  auto it = keymaps.find(str(name));
  return it == keymaps.end() ? nullptr : it->second;
}

str Zle::keymap_name(const sptr<Keymap>& km) const {
  // The non-`main` name, so $KEYMAP says emacs, viins or vicmd.
  str best;
  for (const auto& [name, m] : keymaps) {
    if (m != km) continue;
    if (name != "main" and (best.empty() or best == "main" or name < best)) best = name;
    else if (best.empty()) best = name;
  }
  return best;
}

bool Zle::link(strv existing, strv new_name) {
  auto km = keymap(existing);
  if (not km or new_name == ".safe") return false;
  keymaps[str(new_name)] = km;
  return true;
}

bool Zle::create(strv name, strv from) {
  if (name == ".safe") return false;
  sptr<Keymap> km;
  if (from.empty()) {
    km = s::make_shared<Keymap>();
  } else {
    auto src = keymap(from);
    if (not src) return false;
    km = s::make_shared<Keymap>(*src);
  }
  keymaps[str(name)] = km;
  return true;
}

const vec<str>& Zle::builtin_widgets() {
  static const vec<str> names = [] {
    vec<str> v = Editor::builtin_widget_names();
    s::sort(v.begin(), v.end());
    return v;
  }();
  return names;
}

bool Zle::is_builtin_widget(strv name) {
  const auto& v = builtin_widgets();
  return s::binary_search(v.begin(), v.end(), str(name));
}

str Zle::resolve_widget(strv name) const {
  str n(name);
  for (int hops = 0; hops < 16; ++hops) {
    if (user_widgets.contains(n)) return n;
    if (module_widgets.contains(n)) return n;
    if (auto a = widget_aliases.find(n); a != widget_aliases.end()) {
      n = a->second;
      continue;
    }
    // `.name` is always the builtin, even when `name` was redefined.
    if (n.starts_with('.') and is_builtin_widget(strv(n).substr(1))) return n;
    if (is_builtin_widget(n)) return n;
    return {};
  }
  return {};
}

}  // namespace jfsh
