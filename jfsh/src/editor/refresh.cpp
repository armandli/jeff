#include <jfsh/editor/editor.h>

// The display: the prompt, the buffer and RPROMPT, then any message or
// completion list below them.  Every refresh draws it all again from the
// prompt's first row (cursor up, \e[J, the text, cursor back): simple, and
// right for prompts and buffers over several lines (B26).  Rows are broken
// by the editor, not left to the terminal's auto-margin, so where the
// cursor ends up is always known; a row that fills the width exactly is
// followed by a fresh one.  The width is read again on every refresh, so
// a SIGWINCH just means drawing again (B27).

#include <cstdio>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/core/utf8.h>

namespace jfsh {

int Editor::terminal_width() const noexcept {
  struct winsize ws{};
  if (::ioctl(mOut, TIOCGWINSZ, &ws) == 0 and ws.ws_col > 0) return ws.ws_col;
  str c = mZle.param("COLUMNS");
  int n = c.empty() ? 0 : s::atoi(c.c_str());
  return n > 0 ? n : 80;
}

int Editor::terminal_height() const noexcept {
  struct winsize ws{};
  if (::ioctl(mOut, TIOCGWINSZ, &ws) == 0 and ws.ws_row > 0) return ws.ws_row;
  str l = mZle.param("LINES");
  int n = l.empty() ? 0 : s::atoi(l.c_str());
  return n > 0 ? n : 24;
}

namespace {

// Lays text out in rows of `cols` columns.
struct Layout {
  int cols;
  int row = 0, col = 0;
  str out;

  void newline() {
    out += "\r\n";
    ++row;
    col = 0;
  }
  void put(strv bytes, int width) {
    if (col + width > cols and col > 0) newline();
    out += bytes;
    col += width;
    if (col >= cols) newline();
  }
  void zero_width(strv bytes) { out += bytes; }
};

// Appends a prompt (expanded, with zero-width markers) to the layout.
void lay_out_prompt(Layout& lay, strv p) {
  for (s::size_t i = 0; i < p.size();) {
    char c = p[i];
    if (c == PROMPT_ZW_START) {
      auto end = p.find(PROMPT_ZW_END, i);
      if (end == strv::npos) end = p.size();
      lay.zero_width(p.substr(i + 1, end - i - 1));
      i = end + 1;
      continue;
    }
    if (c == '\x1b') {   // a raw escape sequence: no width
      s::size_t j = i + 1;
      if (j < p.size() and p[j] == '[') {
        ++j;
        while (j < p.size() and not (p[j] >= 0x40 and p[j] <= 0x7e)) ++j;
      }
      if (j < p.size()) ++j;
      lay.zero_width(p.substr(i, j - i));
      i = j;
      continue;
    }
    if (c == '\n') {
      lay.newline();
      ++i;
      continue;
    }
    if (c == '\r' or c == '\a') { ++i; continue; }
    s::size_t len;
    char32_t ch = utf8_decode(p, i, len);
    lay.put(p.substr(i, len), ch < 0x20 ? 0 : char_width(ch));
    i += len;
  }
}

}  // namespace

void Editor::refresh() {
  int cols = s::max(terminal_width(), 2);
  str seq;
  if (mDrawn) {
    if (mCursorRow > 0) seq += "\x1b[" + s::to_string(mCursorRow) + "A";
  }
  seq += "\r\x1b[J";
  mCols = cols;

  Layout lay{cols, 0, 0, {}};
  lay_out_prompt(lay, mPromptText);
  int prompt_row = lay.row;

  // The buffer; control characters show as ^X, the region in standout.
  int cur_row = -1, cur_col = 0;
  s::size_t region_from = 0, region_to = 0;
  if (mRegionActive and mMark) {
    region_from = s::min(*mMark, mBuf.cursor);
    region_to   = s::max(*mMark, mBuf.cursor);
    if (mLocalKeymap and region_to < mBuf.text.size()) region_to = next_char(mBuf.text, region_to);
    if (mRegionLines) {
      region_from = line_start(region_from);
      region_to   = line_end(region_to);
    }
  }
  bool standout = false;
  const str& t = mBuf.text;
  for (s::size_t i = 0; i < t.size();) {
    if (region_to > region_from) {
      bool in = i >= region_from and i < region_to;
      if (in != standout) {
        lay.zero_width(in ? "\x1b[7m" : "\x1b[27m");
        standout = in;
      }
    }
    if (i == mBuf.cursor) {
      if (lay.col >= cols) lay.newline();
      cur_row = lay.row;
      cur_col = lay.col;
    }
    s::size_t len;
    char32_t c = utf8_decode(t, i, len);
    if (c == '\n') {
      lay.newline();
    } else if (c < 0x20 or c == 0x7f) {
      char caret[3] = {'^', static_cast<char>(c == 0x7f ? '?' : (c | 0x40)), 0};
      lay.put(caret, 2);
    } else {
      int w = char_width(c);
      if (w == 0 and lay.col == 0) w = 1;   // a stray combining mark
      lay.put(strv(t).substr(i, len), w);
    }
    i += len;
  }
  if (standout) lay.zero_width("\x1b[27m");
  if (cur_row < 0) {
    cur_row = lay.row;
    cur_col = lay.col;
  }
  int end_row = lay.row, end_col = lay.col;

  // RPROMPT on the prompt's last row, when the line fits beside it.
  bool final_draw = mFinalDraw;
  if (not mRPromptText.empty() and not (final_draw and mOpts.get(OPT_TRANSIENTRPROMPT))) {
    int rw = PromptRenderer::visual_width(mRPromptText);
    int at = cols - rw - 1;
    if (end_row == prompt_row and end_col < at and mRPromptText.find('\n') == str::npos) {
      lay.out += "\x1b[" + s::to_string(at - end_col) + "C";
      lay.out += PromptRenderer::strip_markers(mRPromptText);
      end_col = at + rw;
    }
  }
  if (final_draw) {
    cur_row = end_row;
    cur_col = end_col;
  }

  // Below the line: the message, then the listing, cut to the width.
  str below = mMessage;
  if (not mListing.empty() and not final_draw) {
    if (not below.empty()) below += '\n';
    below += mListing;
  }
  if (not below.empty() and not final_draw) {
    s::size_t at = 0;
    while (at <= below.size()) {
      auto nl = below.find('\n', at);
      strv line = strv(below).substr(at, nl == str::npos ? str::npos : nl - at);
      at = nl == str::npos ? below.size() + 1 : nl + 1;
      lay.newline();
      int w = 0;
      s::size_t cut = 0;
      for (s::size_t i = 0; i < line.size();) {
        if (line[i] == '\x1b') {   // escape sequences in a listing take no room
          s::size_t j = i + 1;
          if (j < line.size() and line[j] == '[') {
            ++j;
            while (j < line.size() and not (line[j] >= 0x40 and line[j] <= 0x7e)) ++j;
          }
          i = cut = s::min(j + 1, line.size());
          continue;
        }
        s::size_t len;
        int cw = char_width(utf8_decode(line, i, len));
        if (w + cw > cols - 1) break;
        w += cw;
        i += len;
        cut = i;
      }
      lay.out += line.substr(0, cut);
      lay.out += "\x1b[0m";
      lay.col = w;
      end_row = lay.row;
      end_col = lay.col;
    }
  }

  seq += lay.out;
  // From the end of what was written back to the cursor.
  if (end_row > cur_row) seq += "\x1b[" + s::to_string(end_row - cur_row) + "A";
  seq += "\r";
  if (cur_col > 0) seq += "\x1b[" + s::to_string(cur_col) + "C";
  mCursorRow = cur_row;
  mDrawn     = true;
  write_out(seq);
}

void Editor::finish_display() {
  mMessage.clear();
  mFinalDraw = true;
  refresh();
  mFinalDraw = false;
  mListing.clear();
  write_out("\r\n");
  mDrawn     = false;
  mCursorRow = 0;
}

void Editor::trash() {
  if (not mDrawn) return;
  str seq;
  if (mCursorRow > 0) seq += "\x1b[" + s::to_string(mCursorRow) + "A";
  seq += "\r\x1b[J";
  write_out(seq);
  mDrawn     = false;
  mCursorRow = 0;
}

}  // namespace jfsh
