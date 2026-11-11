#include <jfsh/editor/editor.hh>

// Vi-mode command dispatch (normal-mode motions, operators, text objects).
//
// TODO(vi/modes): Track vi mode state: INSERT vs NORMAL vs VISUAL vs REPLACE.
//   On ESC in INSERT mode → switch to NORMAL; on 'i','a','I','A','o','O' in
//   NORMAL → switch to INSERT at the appropriate cursor position.
//   Show mode indicator in the prompt (zsh VIMODE or custom widget).
//
// TODO(vi/motions): Implement NORMAL-mode cursor motions:
//   h j k l  — basic
//   w W b B e E  — word motions (small-word vs WORD)
//   ^ $ 0    — line start/end
//   f F t T  — find character on line; ; , repeat
//   / ?      — incremental search forward/backward through history
//   n N      — repeat search
//   G gg     — first/last history entry (like H/L in zsh vi)
//   %        — match bracket
//
// TODO(vi/operators): Implement operators that take a motion:
//   d (delete), c (change), y (yank), ~ (toggle case), gu gU (case)
//   > < (indent)
//   The operator+motion pairs must respect the kill ring / yank buffer.
//
// TODO(vi/textobjects): iw aw (inner/around word), i" a" i' a' i( a( etc.
//
// TODO(vi/register): Implement named registers "a – "z and the " default.
//   p P to paste before/after cursor from register.
//
// TODO(vi/history): Implement k/j to navigate history in NORMAL mode
//   (mirrors zsh's up-line-or-history / down-line-or-history widgets).
//
// TODO(vi/undo): Implement u (undo) and ctrl-r (redo) within the current line.
//   Each INSERT-mode editing session before ESC is one undo unit.
//
// TODO(vi/count): Implement numeric prefix counts (3w, 2dd, 10j, etc.).

namespace jfsh {
}  // namespace jfsh
