/* theme.h — Catppuccin Mocha. The only theme.
 *
 * No switcher, no templates, no `themes/` directory: the palette is compiled in,
 * matching the rest of nemesis (see its CLAUDE.md). Teal is the accent.
 */

#ifndef NEMDOWN_THEME_H
#define NEMDOWN_THEME_H

#include <cairo.h>
#include <stdint.h>

/* ---- the official Mocha palette ---------------------------------------- */
#define CTP_ROSEWATER 0xf5e0dc
#define CTP_FLAMINGO  0xf2cdcd
#define CTP_PINK      0xf5c2e7
#define CTP_MAUVE     0xcba6f7
#define CTP_RED       0xf38ba8
#define CTP_MAROON    0xeba0ac
#define CTP_PEACH     0xfab387
#define CTP_YELLOW    0xf9e2af
#define CTP_GREEN     0xa6e3a1
#define CTP_TEAL      0x94e2d5 /* accent */
#define CTP_SKY       0x89dceb
#define CTP_SAPPHIRE  0x74c7ec
#define CTP_BLUE      0x89b4fa
#define CTP_LAVENDER  0xb4befe

#define CTP_TEXT      0xcdd6f4
#define CTP_SUBTEXT1  0xbac2de
#define CTP_SUBTEXT0  0xa6adc8
#define CTP_OVERLAY2  0x9399b2
#define CTP_OVERLAY1  0x7f849c
#define CTP_OVERLAY0  0x6c7086
#define CTP_SURFACE2  0x585b70
#define CTP_SURFACE1  0x45475a
#define CTP_SURFACE0  0x313244
#define CTP_BASE      0x1e1e2e
#define CTP_MANTLE    0x181825
#define CTP_CRUST     0x11111b

/* ---- roles, so call sites read as intent rather than as colour ---------- */
#define ND_BG_CONTENT   CTP_BASE
#define ND_BG_SIDEBAR   CTP_MANTLE
#define ND_BG_CODE      CTP_MANTLE
#define ND_FG_TEXT      CTP_TEXT
#define ND_ACCENT       CTP_TEAL
#define ND_URGENT       CTP_RED
#define ND_MUTED        CTP_OVERLAY0
#define ND_RULE         CTP_SURFACE1
#define ND_DIVIDER      CTP_SURFACE0
#define ND_SELECTION    CTP_SURFACE2

/* ---- fonts ------------------------------------------------------------- */
#define ND_SANS "Hack Nerd Font"
#define ND_MONO "Hack Nerd Font Mono"

/* ---- helpers ----------------------------------------------------------- */
#define ND_R(h) (((h) >> 16 & 0xff) / 255.0)
#define ND_G(h) (((h) >> 8 & 0xff) / 255.0)
#define ND_B(h) (((h) & 0xff) / 255.0)

static inline void nd_src(cairo_t *cr, uint32_t hex) {
  cairo_set_source_rgb(cr, ND_R(hex), ND_G(hex), ND_B(hex));
}

static inline void nd_src_a(cairo_t *cr, uint32_t hex, double alpha) {
  cairo_set_source_rgba(cr, ND_R(hex), ND_G(hex), ND_B(hex), alpha);
}

/* Pango wants 16-bit channels. */
#define ND_P16R(h) (uint16_t)(((h) >> 16 & 0xff) * 257)
#define ND_P16G(h) (uint16_t)(((h) >> 8 & 0xff) * 257)
#define ND_P16B(h) (uint16_t)(((h) & 0xff) * 257)

/* A 1px hairline straddles a half device pixel at fractional scale and blurs.
 * Snapping to the device grid is the only way to keep rules crisp at 1.5x. */
static inline double nd_snap(double v, double scale) {
  return (scale > 0.0) ? (double)(long)(v * scale + 0.5) / scale : v;
}

#endif /* NEMDOWN_THEME_H */
