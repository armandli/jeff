#include <jfsh/modules/nearcolor.h>

#include <cstdio>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>
#include <jfsh/modules/terminfo.h>
#include <jfsh/prompt/prompt.h>

// zsh/nearcolor (M11): truecolour prompt colours shown as the nearest
// palette colour, as zsh's Src/Modules/nearcolor.c.  zsh measures in
// CIELab; a weighted RGB distance ("redmean") picks the same colour for
// all but a few near-ties and needs no floating-point colour conversion.

namespace jfsh {

namespace {

struct Rgb { int r, g, b; };

long distance(Rgb a, Rgb b) {
  long rmean = (a.r + b.r) / 2;
  long dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
  return (((512 + rmean) * dr * dr) >> 8) + 4 * dg * dg + (((767 - rmean) * db * db) >> 8);
}

// xterm's 16 system colours.
constexpr Rgb kAnsi[16] = {
  {0, 0, 0},       {205, 0, 0},   {0, 205, 0},   {205, 205, 0},
  {0, 0, 238},     {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
  {127, 127, 127}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},
  {92, 92, 255},   {255, 0, 255}, {0, 255, 255}, {255, 255, 255},
};

Rgb palette256(int n) {
  if (n < 16) return kAnsi[n];
  if (n < 232) {
    static constexpr int level[6] = {0, 95, 135, 175, 215, 255};
    n -= 16;
    return {level[n / 36], level[(n / 6) % 6], level[n % 6]};
  }
  int v = 8 + 10 * (n - 232);
  return {v, v, v};
}

// The nearest of the first `size` palette colours; the 256-colour search
// skips the 16 system colours, whose values vary between terminals.
int nearest(Rgb c, int size) {
  int best = size == 16 ? 0 : 16;
  long best_d = -1;
  for (int n = best; n < size; ++n) {
    long d = distance(c, palette256(n));
    if (best_d < 0 or d < best_d) { best = n; best_d = d; }
  }
  return best;
}

str sgr(int n, bool fg, int size) {
  char buf[24];
  if (size == 16)
    s::snprintf(buf, sizeof buf, "\x1b[%dm", (n < 8 ? (fg ? 30 : 40) : (fg ? 90 : 100)) + n % 8);
  else
    s::snprintf(buf, sizeof buf, "\x1b[%d;5;%dm", fg ? 38 : 48, n);
  return buf;
}

// How many colours to map to: 256 when $TERM says so (or its terminfo
// entry has at least 256), else 16.
int palette_size(ExecContext& ctx) {
  str term = param_or(ctx.params, "TERM");
  if (term.find("256") != str::npos) return 256;
  return term_colors(term) >= 256 ? 256 : 16;
}

}  // namespace

void register_nearcolor_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/nearcolor", [](ModuleFeatures& out, ExecContext& ctx) {
    ExecContext* ctxp = &ctx;
    truecolor_mapper = [ctxp](unsigned r, unsigned g, unsigned b, bool fg) {
      int size = palette_size(*ctxp);
      Rgb c{static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)};
      return sgr(nearest(c, size), fg, size);
    };
    out.on_unload = [] { truecolor_mapper = nullptr; };
  });
}

}  // namespace jfsh
