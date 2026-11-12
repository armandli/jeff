#include <jfsh/editor/editor.h>

// Terminal display refresh — full ZLE-style screen management.
//
// TODO(refresh/multiline): Handle prompts and buffers that wrap across terminal
//   lines.  Track (row, col) of every character; use ANSI cursor-up sequences
//   to redraw only changed regions rather than clearing the entire line.
//
// TODO(refresh/rprompt): Draw RPROMPT / RPS1 right-aligned on the prompt line.
//   Must erase the right-prompt before accepting a line (TRANSIENT_RPROMPT).
//
// TODO(refresh/sigwinch): On SIGWINCH, re-query terminal dimensions with
//   TIOCGWINSZ, recalculate line wrapping, and redraw.
//
// TODO(refresh/completion): Display the completion menu below the input line:
//   - Single column for small lists
//   - Multi-column packed layout (LIST_PACKED option)
//   - Row-first vs column-first order (LIST_ROWS_FIRST option)
//   - File-type indicators (LIST_TYPES option: / * @ etc.)
//   - Scroll indicator if list doesn't fit on screen
//   After selection, erase the menu and restore the input line.
//
// TODO(refresh/minibuffer): Implement the ZLE minibuffer used by vared,
//   complist menus, and spelling-correction prompts (SPROMPT).
//
// TODO(refresh/highlight): Implement syntax highlighting via ZLE_LINE_ABORTED
//   / highlighters (zsh-syntax-highlighting compatible interface):
//   color commands, strings, errors, keywords differently.

namespace jfsh {
}  // namespace jfsh
