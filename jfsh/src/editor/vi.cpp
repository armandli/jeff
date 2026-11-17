#include <jfsh/editor/editor.h>

// Vi mode: the vicmd widgets (motions, operators, text objects, registers,
// counts, undo/redo, history), visual mode and the viins extras.  Mirrors
// zsh's zle_vi.c and the vi parts of zle_move.c, zle_word.c and
// zle_hist.c.  An operator (d c y gu gU g~ > <) reads the next key
// sequence through the viopp and vicmd keymaps: the same operator again
// works on whole lines, a motion on the text it moves over, a text object
// (iw aw i" a( …) on the text it selects.  In visual mode the operators
// act on the region at once.

#include <cwctype>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/core/utf8.h>

namespace jfsh {

namespace {

char32_t char_at(strv s, s::size_t pos) {
  s::size_t len;
  return pos < s.size() ? utf8_decode(s, pos, len) : 0;
}

bool blank(char32_t c) { return c == ' ' or c == '\t' or c == '\n'; }

// vi's word classes: word characters, other non-blanks, blanks.
int vi_class(char32_t c, bool big) {
  if (blank(c)) return 0;
  if (big) return 1;
  if (c == '_' or (c < 0x80 and s::isalnum(static_cast<int>(c))) or (c >= 0x80 and s::iswalnum(static_cast<wint_t>(c))) or c > 0xff)
    return 1;
  return 2;
}

}  // namespace

bool Editor::in_vicmd() const {
  return mKeymap and mKeymap == mZle.keymap("vicmd");
}

void Editor::vi_clamp_cursor() {
  if (mBuf.cursor > mBuf.text.size()) mBuf.cursor = mBuf.text.size();
  s::size_t start = line_start(mBuf.cursor);
  if (mBuf.cursor > start and mBuf.cursor == line_end(mBuf.cursor))
    mBuf.cursor = prev_char(mBuf.text, mBuf.cursor);
}

void Editor::vi_enter_insert() {
  select_keymap("viins");
  if (mUndoGroup == 0) mUndoGroup = 1;
}

str Editor::vi_read_char() {
  int c = read_byte(-1);
  if (c < 0 or c == 0x1b or c == 0x03) return {};
  str ch(1, static_cast<char>(c));
  unsigned char lead = static_cast<unsigned char>(c);
  s::size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
  while (ch.size() < need) {
    int more = read_byte(50);
    if (more < 0) break;
    ch += static_cast<char>(more);
  }
  return ch;
}

void Editor::vi_store_register(str text, bool linewise) {
  char r = mViRegister;
  mViRegister = 0;
  if (r >= 'A' and r <= 'Z') {
    auto& slot = mViRegisters[static_cast<char>(r - 'A' + 'a')];
    slot.first += text;
    slot.second = slot.second or linewise;
  } else if ((r >= 'a' and r <= 'z') or (r >= '0' and r <= '9')) {
    mViRegisters[r] = {text, linewise};
  }
  if (r == '_') return;   // the black hole
  mKillRing.insert(mKillRing.begin(), s::move(text));
  if (mKillRing.size() > 8) mKillRing.pop_back();
  mViUnnamedLines = linewise;
}

// ---- Entering insert mode ----------------------------------------------------

bool Editor::w_vi_insert() {
  vi_enter_insert();
  return true;
}

bool Editor::w_vi_add_next() {
  if (mBuf.cursor < line_end(mBuf.cursor)) mBuf.cursor = next_char(mBuf.text, mBuf.cursor);
  vi_enter_insert();
  return true;
}

bool Editor::w_vi_add_eol() {
  mBuf.cursor = line_end(mBuf.cursor);
  vi_enter_insert();
  return true;
}

bool Editor::w_vi_insert_bol() {
  w_vi_first_non_blank();
  vi_enter_insert();
  return true;
}

bool Editor::w_vi_open_line_below() {
  mBuf.cursor = line_end(mBuf.cursor);
  mBuf.insert("\n");
  vi_enter_insert();
  return true;
}

bool Editor::w_vi_open_line_above() {
  mBuf.cursor = line_start(mBuf.cursor);
  mBuf.text.insert(mBuf.cursor, "\n");
  vi_enter_insert();
  return true;
}

bool Editor::w_vi_replace() {
  mBuf.overwrite = true;
  vi_enter_insert();
  return true;
}

// ---- Motions -----------------------------------------------------------------

bool Editor::w_vi_forward_char() {
  s::size_t end = line_end(mBuf.cursor);
  // In vicmd the cursor stays on the line's last character, unless the
  // motion is an operator's (`dl` there deletes it).
  s::size_t limit = (in_vicmd() and not mViOperating and end > line_start(mBuf.cursor))
                  ? prev_char(mBuf.text, end) : end;
  if (mBuf.cursor >= limit) return false;
  for (int n = count(); n > 0 and mBuf.cursor < limit; --n)
    mBuf.cursor = next_char(mBuf.text, mBuf.cursor);
  return true;
}

bool Editor::w_vi_backward_char() {
  s::size_t start = line_start(mBuf.cursor);
  if (mBuf.cursor <= start) return false;
  for (int n = count(); n > 0 and mBuf.cursor > start; --n)
    mBuf.cursor = prev_char(mBuf.text, mBuf.cursor);
  return true;
}

s::size_t Editor::vi_word_forward(s::size_t pos, bool big) const {
  strv t = mBuf.text;
  if (pos >= t.size()) return pos;
  int cls = vi_class(char_at(t, pos), big);
  if (cls != 0)
    while (pos < t.size() and vi_class(char_at(t, pos), big) == cls) pos = next_char(t, pos);
  // An operator's motion stops at the end of the line (`dw` on the last
  // word keeps the newline).
  while (pos < t.size() and blank(char_at(t, pos))) {
    if (mViOperating and t[pos] == '\n') break;
    pos = next_char(t, pos);
  }
  return pos;
}

s::size_t Editor::vi_word_end(s::size_t pos, bool big) const {
  strv t = mBuf.text;
  if (pos >= t.size()) return pos;
  pos = next_char(t, pos);
  while (pos < t.size() and blank(char_at(t, pos))) pos = next_char(t, pos);
  if (pos >= t.size()) return prev_char(t, t.size());
  int cls = vi_class(char_at(t, pos), big);
  for (;;) {
    s::size_t n = next_char(t, pos);
    if (n >= t.size() or vi_class(char_at(t, n), big) != cls) break;
    pos = n;
  }
  return pos;
}

s::size_t Editor::vi_word_backward(s::size_t pos, bool big) const {
  strv t = mBuf.text;
  while (pos > 0 and blank(char_at(t, prev_char(t, pos)))) pos = prev_char(t, pos);
  if (pos == 0) return 0;
  int cls = vi_class(char_at(t, prev_char(t, pos)), big);
  while (pos > 0 and vi_class(char_at(t, prev_char(t, pos)), big) == cls) pos = prev_char(t, pos);
  return pos;
}

s::size_t Editor::vi_word_end_backward(s::size_t pos, bool big) const {
  strv t = mBuf.text;
  if (pos == 0) return 0;
  int cls = vi_class(char_at(t, pos), big);
  if (cls != 0)
    while (pos > 0 and vi_class(char_at(t, pos), big) == cls) pos = prev_char(t, pos);
  while (pos > 0 and blank(char_at(t, pos))) pos = prev_char(t, pos);
  return pos;
}

bool Editor::w_vi_forward_word() {
  s::size_t p = mBuf.cursor;
  for (int n = count(); n > 0; --n) p = vi_word_forward(p, false);
  if (p == mBuf.cursor) return false;
  mBuf.cursor = p;
  return true;
}

bool Editor::w_vi_forward_blank_word() {
  s::size_t p = mBuf.cursor;
  for (int n = count(); n > 0; --n) p = vi_word_forward(p, true);
  if (p == mBuf.cursor) return false;
  mBuf.cursor = p;
  return true;
}

bool Editor::w_vi_forward_word_end() {
  s::size_t p = mBuf.cursor;
  for (int n = count(); n > 0; --n) p = vi_word_end(p, false);
  mBuf.cursor = p;
  return true;
}

bool Editor::w_vi_forward_blank_word_end() {
  s::size_t p = mBuf.cursor;
  for (int n = count(); n > 0; --n) p = vi_word_end(p, true);
  mBuf.cursor = p;
  return true;
}

bool Editor::w_vi_backward_word() {
  if (mBuf.cursor == 0) return false;
  for (int n = count(); n > 0; --n) mBuf.cursor = vi_word_backward(mBuf.cursor, false);
  return true;
}

bool Editor::w_vi_backward_blank_word() {
  if (mBuf.cursor == 0) return false;
  for (int n = count(); n > 0; --n) mBuf.cursor = vi_word_backward(mBuf.cursor, true);
  return true;
}

bool Editor::w_vi_backward_word_end() {
  for (int n = count(); n > 0; --n) mBuf.cursor = vi_word_end_backward(mBuf.cursor, false);
  return true;
}

bool Editor::w_vi_backward_blank_word_end() {
  for (int n = count(); n > 0; --n) mBuf.cursor = vi_word_end_backward(mBuf.cursor, true);
  return true;
}

bool Editor::w_vi_beginning_of_line() {
  mBuf.cursor = line_start(mBuf.cursor);
  return true;
}

bool Editor::w_vi_end_of_line() {
  for (int n = count(); n > 1; --n) {
    s::size_t end = line_end(mBuf.cursor);
    if (end >= mBuf.text.size()) break;
    mBuf.cursor = end + 1;
  }
  s::size_t end = line_end(mBuf.cursor);
  mBuf.cursor = end;
  if (not mViOperating and in_vicmd() and end > line_start(end)) mBuf.cursor = prev_char(mBuf.text, end);
  return true;
}

bool Editor::w_vi_first_non_blank() {
  s::size_t p = line_start(mBuf.cursor), end = line_end(mBuf.cursor);
  while (p < end and (mBuf.text[p] == ' ' or mBuf.text[p] == '\t')) ++p;
  mBuf.cursor = p;
  return true;
}

bool Editor::w_vi_digit_or_beginning_of_line() {
  bool counting = mLastWidget == "digit-argument" or mLastWidget == "vi-digit-or-beginning-of-line";
  if (mArg and counting) return w_digit_argument();
  return w_vi_beginning_of_line();
}

bool Editor::vi_find(char32_t c, int dir, bool skip, int n, bool repeat) {
  strv t = mBuf.text;
  s::size_t start = line_start(mBuf.cursor), end = line_end(mBuf.cursor);
  s::size_t pos = mBuf.cursor;
  s::size_t found = pos;
  for (int k = 0; k < n; ++k) {
    s::size_t p = found;
    // A repeated t/T must not find the character it stopped next to.
    if (repeat and skip and k == 0 and dir > 0 and next_char(t, p) < end and char_at(t, next_char(t, p)) == c) p = next_char(t, p);
    if (repeat and skip and k == 0 and dir < 0 and p > start and char_at(t, prev_char(t, p)) == c) p = prev_char(t, p);
    bool hit = false;
    if (dir > 0) {
      for (p = next_char(t, p); p < end; p = next_char(t, p))
        if (char_at(t, p) == c) { hit = true; break; }
    } else {
      while (p > start) {
        p = prev_char(t, p);
        if (char_at(t, p) == c) { hit = true; break; }
      }
    }
    if (not hit) return false;
    found = p;
  }
  if (skip) found = dir > 0 ? prev_char(t, found) : next_char(t, found);
  mBuf.cursor = found;
  return true;
}

namespace {
char32_t decode_first(const str& ch) {
  s::size_t len;
  return ch.empty() ? 0 : utf8_decode(ch, 0, len);
}
}  // namespace

bool Editor::w_vi_find_prev_char() {
  str ch = vi_read_char();
  if (ch.empty()) return false;
  mViFindChar = decode_first(ch);
  mViFindDir  = -1;
  mViFindSkip = false;
  return vi_find(mViFindChar, -1, false, count());
}

bool Editor::w_vi_find_next_char_skip() {
  str ch = vi_read_char();
  if (ch.empty()) return false;
  mViFindChar = decode_first(ch);
  mViFindDir  = 1;
  mViFindSkip = true;
  return vi_find(mViFindChar, 1, true, count());
}

bool Editor::w_vi_find_prev_char_skip() {
  str ch = vi_read_char();
  if (ch.empty()) return false;
  mViFindChar = decode_first(ch);
  mViFindDir  = -1;
  mViFindSkip = true;
  return vi_find(mViFindChar, -1, true, count());
}

bool Editor::w_vi_repeat_find() {
  if (not mViFindDir) return false;
  return vi_find(mViFindChar, mViFindDir, mViFindSkip, count(), true);
}

bool Editor::w_vi_rev_repeat_find() {
  if (not mViFindDir) return false;
  return vi_find(mViFindChar, -mViFindDir, mViFindSkip, count(), true);
}

// ---- Operators ---------------------------------------------------------------

namespace {

bool is_inclusive_motion(strv w) {
  static const uset<str> inclusive = {
    "vi-forward-word-end", "vi-forward-blank-word-end", "vi-find-next-char",
    "vi-find-next-char-skip", "vi-end-of-line", "vi-match-bracket", "vi-repeat-find",
    "vi-rev-repeat-find", "vi-backward-word-end", "vi-backward-blank-word-end"};
  return inclusive.contains(str(w));
}

bool is_line_motion(strv w) {
  static const uset<str> lines = {
    "up-line-or-history", "down-line-or-history", "vi-up-line-or-history",
    "vi-down-line-or-history", "up-line", "down-line", "up-history", "down-history",
    "vi-goto-mark-line", "beginning-of-buffer-or-history", "vi-fetch-history"};
  return lines.contains(str(w));
}

}  // namespace

bool Editor::vi_operator(ViOp op) {
  // Visual mode: the region, at once.
  if (mRegionActive and mLocalKeymap and mMark) {
    s::size_t from = s::min(*mMark, mBuf.cursor), to = s::max(*mMark, mBuf.cursor);
    if (to < mBuf.text.size()) to = next_char(mBuf.text, to);
    bool lines = mRegionLines;
    mRegionActive = false;
    mRegionLines  = false;
    mLocalKeymap.reset();
    vi_apply(op, from, to, lines);
    return true;
  }
  int n1 = count();
  str self = mZle.resolve_widget(mWidget);
  auto saved_local = mLocalKeymap;
  mLocalKeymap = mZle.keymap("viopp");
  auto saved_keymap = mKeymap;
  mKeymap = mZle.keymap("vicmd");
  // The motion, possibly after a count of its own (d2w).
  const KeyBinding* b = nullptr;
  str seq;
  str w;
  opt<int> n2;
  for (;;) {
    seq = read_binding(b);
    if (not b or b->is_macro) break;
    w = mZle.resolve_widget(b->widget);
    if (w == "digit-argument" or (w == "vi-digit-or-beginning-of-line" and n2)) {
      char d = seq.back();
      n2 = (n2 ? *n2 * 10 : 0) + (d - '0');
      continue;
    }
    break;
  }
  mLocalKeymap = saved_local;
  mKeymap = saved_keymap;
  if (not b or b->is_macro or w.empty() or w == "vi-cmd-mode" or w == "beep") return false;
  int n = n1 * (n2 ? *n2 : 1);

  s::size_t start = mBuf.cursor;
  // The operator twice (dd, cc, yy, gUU …): whole lines.
  if (w == self) {
    s::size_t from = line_start(start), to = from;
    for (int k = 0; k < n; ++k) {
      to = line_end(to);
      if (k + 1 < n) {
        if (to >= mBuf.text.size()) break;
        ++to;
      }
    }
    vi_apply(op, from, to, true);
    return true;
  }
  // j and k as motions cover lines, without going through the history.
  if (w == "down-line-or-history" or w == "vi-down-line-or-history" or w == "down-line"
      or w == "up-line-or-history" or w == "vi-up-line-or-history" or w == "up-line") {
    bool down = w.find("down") != str::npos;
    s::size_t from = line_start(start), to = line_end(start);
    for (int k = 0; k < n; ++k) {
      if (down) {
        if (to >= mBuf.text.size()) return false;
        to = line_end(to + 1);
      } else {
        if (from == 0) return false;
        from = line_start(from - 1);
      }
    }
    vi_apply(op, from, to, true);
    return true;
  }

  mViObject.reset();
  mViObjectLines = false;
  mViOperating   = true;
  mArg  = n;
  mKeys = seq;
  // `cw` on a word changes to the end of it, like `ce`.
  str motion = w;
  if (op == ViOp::Change and (w == "vi-forward-word" or w == "vi-forward-blank-word")
      and start < mBuf.text.size() and not blank(char_at(mBuf.text, start))) {
    bool big = w == "vi-forward-blank-word";
    s::size_t p = start;
    for (int k = 0; k < n; ++k) {
      // On a word's last character already, `cw` changes just that.
      s::size_t next = next_char(mBuf.text, p);
      bool at_end = next >= mBuf.text.size()
                 or vi_class(char_at(mBuf.text, next), big) != vi_class(char_at(mBuf.text, p), big);
      if (k > 0 or not at_end) p = vi_word_end(p, big);
    }
    mBuf.cursor = p;
    motion = "vi-forward-word-end";
  } else if (not dispatch(w)) {
    mViOperating = false;
    mBuf.cursor = start;
    return false;
  }
  mViOperating = false;
  s::size_t from, to;
  bool lines = false;
  if (mViObject) {
    from  = mViObject->first;
    to    = mViObject->second;
    lines = mViObjectLines;
    mViObject.reset();
  } else {
    s::size_t end = mBuf.cursor;
    from = s::min(start, end);
    to   = s::max(start, end);
    if (is_inclusive_motion(motion) and to < mBuf.text.size() and mBuf.text[to] != '\n')
      to = next_char(mBuf.text, to);
    lines = is_line_motion(motion);
    if (lines) {
      from = line_start(from);
      to   = line_end(to);
    }
  }
  vi_apply(op, from, to, lines);
  return true;
}

void Editor::vi_apply(ViOp op, s::size_t from, s::size_t to, bool linewise) {
  to = s::min(to, mBuf.text.size());
  str& t = mBuf.text;
  switch (op) {
    case ViOp::Yank:
      vi_store_register(t.substr(from, to - from), linewise);
      mBuf.cursor = from;
      break;
    case ViOp::Delete: {
      vi_store_register(t.substr(from, to - from), linewise);
      s::size_t a = from, b = to;
      if (linewise) {   // the line break goes too
        if (b < t.size()) ++b;
        else if (a > 0) --a;
      }
      t.erase(a, b - a);
      mBuf.cursor = s::min(a, t.size());
      if (linewise) {
        if (mBuf.cursor > 0 and mBuf.cursor >= t.size()) mBuf.cursor = line_start(mBuf.cursor);
        w_vi_first_non_blank();
      }
      break;
    }
    case ViOp::Change:
      vi_store_register(t.substr(from, to - from), linewise);
      t.erase(from, to - from);
      mBuf.cursor = from;
      vi_enter_insert();
      break;
    case ViOp::Upper: case ViOp::Lower: case ViOp::Swap: {
      str out;
      for (s::size_t i = from; i < to;) {
        s::size_t len;
        char32_t c = utf8_decode(t, i, len);
        wint_t wc = static_cast<wint_t>(c);
        if (op == ViOp::Upper)      c = static_cast<char32_t>(s::towupper(wc));
        else if (op == ViOp::Lower) c = static_cast<char32_t>(s::towlower(wc));
        else c = static_cast<char32_t>(s::iswupper(wc) ? s::towlower(wc) : s::towupper(wc));
        utf8_append(out, c);
        i += len;
      }
      t.replace(from, to - from, out);
      mBuf.cursor = from;
      break;
    }
    case ViOp::Indent: case ViOp::Unindent: {
      s::size_t p = line_start(from);
      for (;;) {
        if (op == ViOp::Indent) {
          if (p < t.size() and t[p] != '\n') {
            t.insert(p, "\t");
            to += 1;
          }
        } else if (p < t.size() and t[p] == '\t') {
          t.erase(p, 1);
          to -= 1;
        }
        s::size_t e = line_end(p);
        if (e >= to or e >= t.size()) break;
        p = e + 1;
      }
      mBuf.cursor = line_start(from);
      w_vi_first_non_blank();
      break;
    }
  }
  if (in_vicmd()) vi_clamp_cursor();
}

bool Editor::w_vi_delete()        { return vi_operator(ViOp::Delete); }
bool Editor::w_vi_change()        { return vi_operator(ViOp::Change); }
bool Editor::w_vi_yank()          { return vi_operator(ViOp::Yank); }
bool Editor::w_vi_up_case()       { return vi_operator(ViOp::Upper); }
bool Editor::w_vi_down_case()     { return vi_operator(ViOp::Lower); }
bool Editor::w_vi_oper_swap_case(){ return vi_operator(ViOp::Swap); }
bool Editor::w_vi_indent()        { return vi_operator(ViOp::Indent); }
bool Editor::w_vi_unindent()      { return vi_operator(ViOp::Unindent); }

bool Editor::w_vi_delete_char() {
  s::size_t end = line_end(mBuf.cursor);
  if (mBuf.cursor >= end) return false;
  s::size_t to = mBuf.cursor;
  for (int n = count(); n > 0 and to < end; --n) to = next_char(mBuf.text, to);
  vi_apply(ViOp::Delete, mBuf.cursor, to, false);
  return true;
}

bool Editor::w_vi_backward_delete_char() {
  s::size_t start = in_vicmd() ? line_start(mBuf.cursor) : 0;
  if (mBuf.cursor <= start) return false;
  s::size_t from = mBuf.cursor;
  for (int n = count(); n > 0 and from > start; --n) from = prev_char(mBuf.text, from);
  if (in_vicmd()) {
    vi_apply(ViOp::Delete, from, mBuf.cursor, false);
  } else {
    mBuf.text.erase(from, mBuf.cursor - from);
    mBuf.cursor = from;
  }
  return true;
}

bool Editor::w_vi_kill_eol() {
  s::size_t end = line_end(mBuf.cursor);
  if (mBuf.cursor >= end) return false;
  vi_apply(ViOp::Delete, mBuf.cursor, end, false);
  return true;
}

bool Editor::w_vi_change_eol() {
  vi_apply(ViOp::Change, mBuf.cursor, line_end(mBuf.cursor), false);
  return true;
}

bool Editor::w_vi_change_whole_line() {
  s::size_t from = line_start(mBuf.cursor);
  vi_apply(ViOp::Change, from, line_end(mBuf.cursor), true);
  return true;
}

bool Editor::w_vi_substitute() {
  s::size_t end = line_end(mBuf.cursor), to = mBuf.cursor;
  for (int n = count(); n > 0 and to < end; --n) to = next_char(mBuf.text, to);
  vi_apply(ViOp::Change, mBuf.cursor, to, false);
  return true;
}

bool Editor::w_vi_yank_whole_line() {
  s::size_t from = line_start(mBuf.cursor), to = from;
  for (int n = count(); n > 0; --n) {
    to = line_end(to);
    if (n > 1 and to < mBuf.text.size()) ++to;
  }
  s::size_t keep = mBuf.cursor;
  vi_apply(ViOp::Yank, from, to, true);
  mBuf.cursor = keep;
  return true;
}

bool Editor::w_vi_put_after() {
  str text;
  bool lines;
  if (mViRegister and mViRegister != '"') {
    char r = static_cast<char>(s::tolower(mViRegister));
    auto it = mViRegisters.find(r);
    mViRegister = 0;
    if (it == mViRegisters.end()) return false;
    text  = it->second.first;
    lines = it->second.second;
  } else {
    mViRegister = 0;
    if (mKillRing.empty()) return false;
    text  = mKillRing.front();
    lines = mViUnnamedLines;
  }
  str all;
  for (int n = count(); n > 0; --n) all += (lines and not all.empty() ? "\n" : "") + text;
  if (lines) {
    s::size_t end = line_end(mBuf.cursor);
    mBuf.text.insert(end, "\n" + all);
    mBuf.cursor = end + 1;
    w_vi_first_non_blank();
  } else {
    if (mBuf.cursor < line_end(mBuf.cursor)) mBuf.cursor = next_char(mBuf.text, mBuf.cursor);
    mBuf.text.insert(mBuf.cursor, all);
    mBuf.cursor += all.size();
    if (mBuf.cursor > 0) mBuf.cursor = prev_char(mBuf.text, mBuf.cursor);
  }
  return true;
}

bool Editor::w_vi_put_before() {
  str text;
  bool lines;
  if (mViRegister and mViRegister != '"') {
    char r = static_cast<char>(s::tolower(mViRegister));
    auto it = mViRegisters.find(r);
    mViRegister = 0;
    if (it == mViRegisters.end()) return false;
    text  = it->second.first;
    lines = it->second.second;
  } else {
    mViRegister = 0;
    if (mKillRing.empty()) return false;
    text  = mKillRing.front();
    lines = mViUnnamedLines;
  }
  str all;
  for (int n = count(); n > 0; --n) all += (lines and not all.empty() ? "\n" : "") + text;
  if (lines) {
    s::size_t start = line_start(mBuf.cursor);
    mBuf.text.insert(start, all + "\n");
    mBuf.cursor = start;
    w_vi_first_non_blank();
  } else {
    mBuf.text.insert(mBuf.cursor, all);
    mBuf.cursor += all.size();
    if (mBuf.cursor > 0) mBuf.cursor = prev_char(mBuf.text, mBuf.cursor);
  }
  return true;
}

bool Editor::w_put_replace_selection() {
  if (not (mRegionActive and mMark) or mKillRing.empty()) return false;
  str text = mKillRing.front();
  s::size_t from = s::min(*mMark, mBuf.cursor), to = s::max(*mMark, mBuf.cursor);
  if (to < mBuf.text.size()) to = next_char(mBuf.text, to);
  str old = mBuf.text.substr(from, to - from);
  mBuf.text.replace(from, to - from, text);
  mBuf.cursor = from + text.size();
  if (mBuf.cursor > from) mBuf.cursor = prev_char(mBuf.text, mBuf.cursor);
  mRegionActive = false;
  mLocalKeymap.reset();
  vi_store_register(old, false);
  return true;
}

bool Editor::w_vi_replace_chars() {
  str ch = vi_read_char();
  if (ch.empty()) return false;
  if (ch == "\r") ch = "\n";
  s::size_t end = line_end(mBuf.cursor), to = mBuf.cursor;
  int n = count();
  for (int k = 0; k < n; ++k) {
    if (to >= end) return false;
    to = next_char(mBuf.text, to);
  }
  str rep;
  for (int k = 0; k < n; ++k) rep += ch;
  mBuf.text.replace(mBuf.cursor, to - mBuf.cursor, rep);
  mBuf.cursor += rep.size();
  mBuf.cursor = prev_char(mBuf.text, mBuf.cursor);
  return true;
}

bool Editor::w_vi_swap_case() {
  s::size_t end = line_end(mBuf.cursor);
  if (mBuf.cursor >= end) return false;
  s::size_t to = mBuf.cursor;
  for (int n = count(); n > 0 and to < end; --n) to = next_char(mBuf.text, to);
  vi_apply(ViOp::Swap, mBuf.cursor, to, false);
  mBuf.cursor = s::min(to, end);
  if (in_vicmd()) vi_clamp_cursor();
  return true;
}

bool Editor::w_vi_set_buffer() {
  str ch = vi_read_char();
  if (ch.size() != 1) return false;
  char r = ch[0];
  if (not (s::isalnum(static_cast<unsigned char>(r)) or r == '"' or r == '_')) return false;
  mViRegister = r;
  mArgSet = mArg.has_value();   // a count before the register carries on
  return true;
}

bool Editor::w_vi_set_mark() {
  str ch = vi_read_char();
  if (ch.size() != 1 or not s::isalpha(static_cast<unsigned char>(ch[0]))) return false;
  mViMarks[ch[0]] = mBuf.cursor;
  return true;
}

bool Editor::w_vi_goto_mark() {
  str ch = vi_read_char();
  if (ch.size() != 1) return false;
  auto it = mViMarks.find(ch[0]);
  if (it == mViMarks.end()) return false;
  mBuf.cursor = s::min(it->second, mBuf.text.size());
  return true;
}

bool Editor::w_vi_goto_mark_line() {
  if (not w_vi_goto_mark()) return false;
  return w_vi_first_non_blank();
}

// ---- Insert-mode extras --------------------------------------------------------

bool Editor::w_vi_kill_line() {
  s::size_t start = line_start(mBuf.cursor);
  if (start == mBuf.cursor) return false;
  kill_text(start, mBuf.cursor, true);
  return true;
}

bool Editor::w_vi_backward_kill_word() {
  s::size_t start = mBuf.cursor;
  for (int n = count(); n > 0; --n) start = vi_word_backward(start, false);
  if (start == mBuf.cursor) return false;
  kill_text(start, mBuf.cursor, true);
  return true;
}

bool Editor::w_vi_quoted_insert() { return w_quoted_insert(); }

// ---- History -----------------------------------------------------------------

bool Editor::w_vi_up_line_or_history() {
  if (not w_up_line_or_history()) return false;
  return w_vi_first_non_blank();
}

bool Editor::w_vi_down_line_or_history() {
  if (not w_down_line_or_history()) return false;
  return w_vi_first_non_blank();
}

bool Editor::w_vi_fetch_history() {
  const auto& e = mHist.entries();
  if (not mArg) {
    set_history_line(static_cast<int>(e.size()));
  } else {
    int idx = -1;
    for (s::size_t i = 0; i < e.size(); ++i)
      if (static_cast<int>(e[i].num) == *mArg) idx = static_cast<int>(i);
    if (idx < 0) return false;
    set_history_line(idx);
  }
  mBuf.cursor = 0;
  return true;
}

bool Editor::vi_history_search(int dir, bool ask) {
  if (ask) {
    auto pat = minibuffer(dir < 0 ? "/" : "?");
    if (not pat) return false;
    if (not pat->empty()) mViSearch = *pat;
    mViSearchDir = dir;
  }
  if (mViSearch.empty()) return false;
  const auto& e = mHist.entries();
  int size = static_cast<int>(e.size());
  for (int i = mHistIndex + dir; i >= 0 and i < size; i += dir) {
    if (e[static_cast<s::size_t>(i)].text.find(mViSearch) != str::npos
        and e[static_cast<s::size_t>(i)].text != mBuf.text) {
      set_history_line(i);
      mBuf.cursor = 0;
      return true;
    }
  }
  return false;
}

bool Editor::w_vi_history_search_backward() { return vi_history_search(-1, true); }
bool Editor::w_vi_history_search_forward()  { return vi_history_search(1, true); }
bool Editor::w_vi_repeat_search()     { return vi_history_search(mViSearchDir, false); }
bool Editor::w_vi_rev_repeat_search() { return vi_history_search(-mViSearchDir, false); }

// ---- Repeating, visual mode ------------------------------------------------------

bool Editor::w_vi_repeat_change() {
  if (mViLastChange.empty() or mViReplaying) return false;
  str saved = s::move(mPending);
  mPending = mViLastChange;
  mViReplaying = true;
  bool saved_record = mViRecord;
  mViRecord = false;
  while (not mPending.empty() and not mDone) {
    const KeyBinding* b = nullptr;
    str seq = read_binding(b);
    if (seq.empty()) break;
    mKeys = seq;
    if (b and b->is_macro) push_input(b->macro);
    else if (b) run_widget(b->widget);
    else if (mKeymap->insert_unbound) run_widget("self-insert");
  }
  mViReplaying = false;
  mViRecord = saved_record;
  mPending = s::move(saved) + mPending;
  return true;
}

bool Editor::w_visual_mode() {
  if (mRegionActive and mLocalKeymap and not mRegionLines) return w_deactivate_region();
  mMark = mBuf.cursor;
  mRegionActive = true;
  mRegionLines  = false;
  mLocalKeymap  = mZle.keymap("visual");
  return true;
}

bool Editor::w_visual_line_mode() {
  if (mRegionActive and mLocalKeymap and mRegionLines) {
    mRegionLines = false;
    return w_deactivate_region();
  }
  mMark = mBuf.cursor;
  mRegionActive = true;
  mRegionLines  = true;
  mLocalKeymap  = mZle.keymap("visual");
  return true;
}

// ---- Text objects --------------------------------------------------------------

namespace {
// Sets the selection a text object found: for an operator, the text it
// works on; in visual mode, the region.
}  // namespace

bool Editor::vi_select_word(bool around, bool big) {
  strv t = mBuf.text;
  if (t.empty()) return false;
  s::size_t pos = s::min(mBuf.cursor, t.size() - 1);
  s::size_t ls = line_start(pos), le = line_end(pos);
  int cls = vi_class(char_at(t, pos), big);
  s::size_t from = pos, to = next_char(t, pos);
  while (from > ls and vi_class(char_at(t, prev_char(t, from)), big) == cls) from = prev_char(t, from);
  while (to < le and vi_class(char_at(t, to), big) == cls) to = next_char(t, to);
  if (around) {
    s::size_t b = to;
    while (b < le and (t[b] == ' ' or t[b] == '\t')) ++b;
    if (b > to and cls != 0) {
      to = b;
    } else {
      while (from > ls and (t[from - 1] == ' ' or t[from - 1] == '\t')) --from;
    }
  }
  for (int n = count(); n > 1 and to < le; --n) {   // aw/iw with a count
    int c2 = vi_class(char_at(t, to), big);
    while (to < le and vi_class(char_at(t, to), big) == c2) to = next_char(t, to);
  }
  if (mViOperating) {
    mViObject = s::make_pair(from, to);
    mViObjectLines = false;
  } else {
    mMark = from;
    mBuf.cursor = to > from ? prev_char(t, to) : from;
    mRegionActive = true;
  }
  return true;
}

bool Editor::vi_select_quoted(bool around) {
  strv t = mBuf.text;
  char q = mKeys.empty() ? '"' : mKeys.back();
  s::size_t ls = line_start(mBuf.cursor), le = line_end(mBuf.cursor);
  vec<s::size_t> quotes;
  for (s::size_t i = ls; i < le; ++i) {
    if (t[i] == '\\') { ++i; continue; }
    if (t[i] == q) quotes.push_back(i);
  }
  s::size_t open = str::npos, close = str::npos;
  for (s::size_t k = 0; k + 1 < quotes.size(); k += 2) {
    if (mBuf.cursor >= quotes[k] and mBuf.cursor <= quotes[k + 1]) {
      open = quotes[k];
      close = quotes[k + 1];
      break;
    }
  }
  if (open == str::npos) {   // the next quoted string on the line
    for (s::size_t k = 0; k + 1 < quotes.size(); k += 2)
      if (quotes[k] > mBuf.cursor) { open = quotes[k]; close = quotes[k + 1]; break; }
  }
  if (open == str::npos) return false;
  s::size_t from = around ? open : open + 1, to = around ? close + 1 : close;
  if (mViOperating) {
    mViObject = s::make_pair(from, to);
  } else {
    mMark = from;
    mBuf.cursor = to > from ? to - 1 : from;
    mRegionActive = true;
  }
  return true;
}

bool Editor::vi_select_bracketed(bool around) {
  strv t = mBuf.text;
  char k = mKeys.empty() ? '(' : mKeys.back();
  char open, close;
  switch (k) {
    case '(': case ')': case 'b': open = '('; close = ')'; break;
    case '[': case ']':           open = '['; close = ']'; break;
    case '{': case '}': case 'B': open = '{'; close = '}'; break;
    default:                      open = '<'; close = '>'; break;
  }
  s::size_t pos = s::min(mBuf.cursor, t.empty() ? 0 : t.size() - 1);
  // The innermost pair around the cursor.
  s::size_t from = str::npos;
  int depth = 0;
  for (s::size_t i = pos + 1; i-- > 0;) {
    if (t[i] == close and i != pos) ++depth;
    else if (t[i] == open) {
      if (depth == 0) { from = i; break; }
      --depth;
    }
  }
  if (from == str::npos) return false;
  s::size_t to = str::npos;
  depth = 0;
  for (s::size_t i = from + 1; i < t.size(); ++i) {
    if (t[i] == open) ++depth;
    else if (t[i] == close) {
      if (depth == 0) { to = i; break; }
      --depth;
    }
  }
  if (to == str::npos) return false;
  s::size_t a = around ? from : from + 1, b = around ? to + 1 : to;
  if (mViOperating) {
    mViObject = s::make_pair(a, b);
  } else {
    mMark = a;
    mBuf.cursor = b > a ? b - 1 : a;
    mRegionActive = true;
  }
  return true;
}

bool Editor::w_select_in_word()        { return vi_select_word(false, false); }
bool Editor::w_select_a_word()         { return vi_select_word(true, false); }
bool Editor::w_select_in_blank_word()  { return vi_select_word(false, true); }
bool Editor::w_select_a_blank_word()   { return vi_select_word(true, true); }
bool Editor::w_select_in_shell_word()  { return vi_select_word(false, true); }
bool Editor::w_select_a_shell_word()   { return vi_select_word(true, true); }
bool Editor::w_select_in_quoted()      { return vi_select_quoted(false); }
bool Editor::w_select_a_quoted()       { return vi_select_quoted(true); }
bool Editor::w_select_in_bracketed()   { return vi_select_bracketed(false); }
bool Editor::w_select_a_bracketed()    { return vi_select_bracketed(true); }

}  // namespace jfsh
