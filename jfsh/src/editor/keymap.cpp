// The key notation and listing order in this file (getkeystring,
// bindztrdup, dquotedztrdup, scankeymap) are derived from zsh 5.9's
// Src/utils.c and Src/Zle/zle_keymap.c.
// zsh's copyright and licence:
//
// The Z Shell is copyright (c) 1992-2017 Paul Falstad, Richard Coleman,
// Zoltán Hidvégi, Andrew Main, Peter Stephenson, Sven Wischnowsky, and
// others.  All rights reserved.  Individual authors, whether or not
// specifically named, retain copyright in all changes; in what follows, they
// are referred to as `the Zsh Development Group'.  This is for convenience
// only and this body has no legal status.  The Z shell is distributed under
// the following licence; any provisions made in individual files take
// precedence.
//
// Permission is hereby granted, without written agreement and without
// licence or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall the Zsh Development Group be liable to any party for
// direct, indirect, special, incidental, or consequential damages arising out
// of the use of this software and its documentation, even if the Zsh
// Development Group have been advised of the possibility of such damage.
//
// The Zsh Development Group specifically disclaim any warranties, including,
// but not limited to, the implied warranties of merchantability and fitness
// for a particular purpose.  The software provided hereunder is on an "as is"
// basis, and the Zsh Development Group have no obligation to provide
// maintenance, support, updates, enhancements, or modifications.

#include <jfsh/core/aliases.h>
#include <jfsh/core/utf8.h>
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

// zsh's getkeystring with GETKEYS_BINDKEY: backslash escapes (\e, \n,
// \NNN octal, \xNN, \uNNNN …), \M- and \C- and ^ modifying the
// character that follows, whatever escape gives it.
str parse_key_notation(strv t) {
  str out;
  int meta = 0;
  bool control = false;
  for (s::size_t i = 0; i < t.size(); ++i) {
    char c = t[i];
    s::size_t before = out.size();
    if (c == '\\' and i + 1 < t.size()) {
      char e = t[++i];
      switch (e) {
      case 'a': out += '\a';
      break; case 'n': out += '\n';
      break; case 'b': out += '\b';
      break; case 't': out += '\t';
      break; case 'v': out += '\v';
      break; case 'f': out += '\f';
      break; case 'r': out += '\r';
      break; case 'E': case 'e': out += '\x1b';

      break; case 'M':
        if (i + 1 < t.size() and t[i + 1] == '-') ++i;
        meta = 1 + control;   // the order of ^ and \M- matters
        continue;

      break; case 'C':
        if (i + 1 < t.size() and t[i + 1] == '-') ++i;
        control = true;
        continue;

      break; case 'u': case 'U': {
        char32_t w = 0;
        for (int k = e == 'u' ? 4 : 8; k > 0 and i + 1 < t.size()
             and s::isxdigit(static_cast<unsigned char>(t[i + 1])); --k) {
          char h = t[++i];
          w = w * 16 + static_cast<char32_t>(s::isdigit(static_cast<unsigned char>(h)) ? h - '0'
                                              : (s::tolower(h) - 'a' + 10));
        }
        utf8_append(out, w);
      }

      break; default:
        if ((e >= '0' and e <= '7') or e == 'x') {
          // At most three characters after the backslash, the x
          // counting.
          s::size_t end = s::min(t.size(), i + 3);
          bool hex = e == 'x';
          s::size_t j = hex ? i + 1 : i;
          unsigned v = 0;
          while (j < end and (hex ? s::isxdigit(static_cast<unsigned char>(t[j])) != 0
                                  : (t[j] >= '0' and t[j] <= '7'))) {
            char h = t[j++];
            v = v * (hex ? 16U : 8U)
              + static_cast<unsigned>(s::isdigit(static_cast<unsigned char>(h)) ? h - '0'
                                       : (s::tolower(h) - 'a' + 10));
          }
          out += static_cast<char>(v);
          i = j - 1;
        } else {
          out += e;
        }
      }
    } else if (c == '^' and not control and i + 1 < t.size()) {
      control = true;
      continue;
    } else {
      out += c;
    }
    if (out.size() == before) continue;
    auto& last = reinterpret_cast<unsigned char&>(out.back());
    if (meta == 2) {
      last |= 0x80;
      meta = 0;
    }
    if (control) {
      last = last == '?' ? 0x7f : static_cast<unsigned char>(last & 0x9f);
      control = false;
    }
    if (meta) {
      last |= 0x80;
      meta = 0;
    }
  }
  return out;
}

// zsh's bindztrdup: ^X for control characters, \M- for the eighth bit,
// \\ and \^ for themselves.
str bind_text(strv seq) {
  str b;
  for (char ch : seq) {
    auto c = static_cast<unsigned char>(ch);
    if (c & 0x80) {
      b += "\\M-";
      c &= 0x7f;
    }
    if (c < 32 or c == 0x7f) {
      b += '^';
      c ^= 64;
    }
    if (c == '\\' or c == '^') b += '\\';
    b += static_cast<char>(c);
  }
  return b;
}

// zsh's printbind: bind_text inside dquotedztrdup's double quotes (a
// backslash after a backslash is doubled, \" \$ \` escaped).
str format_key(strv seq) {
  str out = "\"";
  bool pending = false;
  for (char c : bind_text(seq)) {
    switch (c) {
    case '\\':
      if (pending) out += '\\';
      out += '\\';
      pending = true;

    break; case '"': case '$': case '`':
      if (pending) out += '\\';
      out += '\\';
      out += c;
      pending = false;

    break; default:
      out += c;
      pending = false;
    }
  }
  if (pending) out += '\\';
  return out + '"';
}

namespace {

// zsh's metafied form of a key sequence, the order bindkey lists the
// sequences of more than one key in (hnamcmp with zsh's ztrcmp, which
// compares plain chars: bytes from 0x80 up sort first).
str metafied(strv seq) {
  str out;
  for (char ch : seq) {
    auto c = static_cast<unsigned char>(ch);
    if (c == 0 or (c >= 0x83 and c <= 0xa2)) {
      out += static_cast<char>(0x83);
      out += static_cast<char>(c ^ 32);
    } else {
      out += ch;
    }
  }
  return out;
}

}  // namespace

vec<const s::pair<const str, KeyBinding>*> Keymap::scan() const {
  vec<const s::pair<const str, KeyBinding>*> multi, out;
  const s::pair<const str, KeyBinding>* single[256] = {};
  for (const auto& kv : bindings) {
    if (kv.first.size() == 1) single[static_cast<unsigned char>(kv.first[0])] = &kv;
    else                      multi.push_back(&kv);
  }
  auto less = [](char x, char y) {
    return static_cast<signed char>(x) < static_cast<signed char>(y);
  };
  s::stable_sort(multi.begin(), multi.end(), [&](const auto* a, const auto* b) {
    str ma = metafied(a->first), mb = metafied(b->first);
    return s::lexicographical_compare(ma.begin(), ma.end(), mb.begin(), mb.end(), less);
  });
  int last = -1;
  for (const auto* m : multi) {
    int f = static_cast<unsigned char>(m->first[0]);
    while (last < f) {
      ++last;
      if (single[last]) out.push_back(single[last]);
    }
    out.push_back(m);
  }
  while (last < 255) {
    ++last;
    if (single[last]) out.push_back(single[last]);
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
  bind_range(*km, 0x80, 0xff, "self-insert");
  bind_terminal_keys(*km, false);
  return km;
}

sptr<Keymap> make_viins() {
  auto km = s::make_shared<Keymap>();
  bind_range(*km, 0x01, 0x1f, "self-insert");
  bind_range(*km, ' ', '~', "self-insert");
  bind_range(*km, 0x80, 0xff, "self-insert");
  km->unbind("\x18");
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
    {"gE", "vi-backward-blank-word-end"}, {"gU", "vi-up-case"}, {"ga", "what-cursor-position"},
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

// zsh's fallback keymap, used when `main` is gone: every byte inserts
// itself but newline and return, which accept the line.
sptr<Keymap> make_safe() {
  auto km = s::make_shared<Keymap>();
  bind_range(*km, 0x00, 0xff, ".self-insert");
  km->bind("\n", ".accept-line");
  km->bind("\r", ".accept-line");
  return km;
}

// The keymap execute-named-cmd and friends read with.
sptr<Keymap> make_command() {
  auto km = s::make_shared<Keymap>();
  km->bind("\x07", "send-break");
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
  keymaps["command"] = make_command();
  keymaps[".safe"]   = make_safe();
  for (auto& [name, km] : keymaps) km->primary = name;
  select_main(vi);
}

sptr<Keymap> Zle::keymap(strv name) const {
  auto it = keymaps.find(str(name));
  return it == keymaps.end() ? nullptr : it->second;
}

str Zle::keymap_name(const sptr<Keymap>& km) const {
  return km ? km->primary : str{};
}

void Zle::link_keymap(const sptr<Keymap>& km, strv name) {
  auto& slot = keymaps[str(name)];
  if (slot == km) return;
  sptr<Keymap> old = s::move(slot);
  slot = km;
  if (km->primary.empty() and name != "main") km->primary = str(name);
  if (old and old->primary == name) pick_primary(old);
}

void Zle::pick_primary(const sptr<Keymap>& km) {
  km->primary.clear();
  vec<str> names;
  for (const auto& [n, m] : keymaps)
    if (m == km and n != "main") names.push_back(n);
  s::sort(names.begin(), names.end());
  if (not names.empty()) km->primary = names.front();
}

bool Zle::link(strv existing, strv new_name) {
  auto km = keymap(existing);
  if (not km or new_name == ".safe") return false;
  link_keymap(km, new_name);
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
  km->primary.clear();
  link_keymap(km, name);
  return true;
}

bool Zle::unlink(strv name) {
  auto it = keymaps.find(str(name));
  if (it == keymaps.end()) return false;
  sptr<Keymap> km = it->second;
  keymaps.erase(it);
  if (km->primary == name) pick_primary(km);
  return true;
}

vec<str> Zle::widget_names() const {
  vec<str> names;
  for (const auto& w : builtin_widgets()) {
    names.push_back("." + w);
    if (not deleted_widgets.contains(w)) names.push_back(w);
  }
  for (const auto& [w, m] : module_widgets) names.push_back(w);
  for (const auto& [w, fn] : user_widgets) names.push_back(w);
  for (const auto& [w, to] : widget_aliases) names.push_back(w);
  s::sort(names.begin(), names.end());
  names.erase(s::unique(names.begin(), names.end()), names.end());
  return names;
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
  if (deleted_widgets.contains(n)) return {};
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
