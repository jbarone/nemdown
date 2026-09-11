# nemdown

A native Wayland markdown viewer for Hyprland, written in C. No Electron, no
browser engine, no toolkit — `libwayland-client` + xdg-shell + `wl_shm`, with
Cairo and Pango doing the drawing.

```
nemdown file.md
```

The window is a split: a sidebar listing the file's frontmatter properties above
its table of contents, and the rendered document beside it. Catppuccin Mocha,
teal accent, no theme switcher.

## Keys

```
j / k / arrows    scroll a line        g / G    top / bottom
PgUp / PgDn       page                 b        toggle the sidebar
q / Esc           quit
```

Clicking an entry in the contents pane scrolls to that heading.

## Building

```
make          # debug build at build/debug/nemdown
make release  # optimised
make asan     # address + UB sanitizers
make test     # headless engine harnesses over test/fixtures
sudo make install
```

Dependencies, all from the Arch repos: `wayland` `wayland-protocols` `cairo`
`pango` `md4c` `libyaml` `libxkbcommon` `gdk-pixbuf2` `librsvg`, plus
`wl-clipboard` at runtime. Fonts: Hack Nerd Font and Hack Nerd Font Mono.

## Rendering

Aims at Obsidian's reading view. Supported: headings, emphasis and nesting,
inline and fenced code with syntax highlighting, `==highlights==`, links and
`[[wikilinks]]`, nested and ordered lists, task checkboxes, blockquotes,
Obsidian callouts (`> [!warning] Title`), tables with alignment, horizontal
rules, YAML frontmatter, and `$math$` styled but not typeset.

Not yet: image decoding (placeholders are drawn), text selection, `/` search,
LaTeX typesetting, footnotes.

## Development tools

Headless, so they need no compositor:

```
build/dump_md4c  <file.md>            # raw md4c callback stream
build/dump_tree  <file.md>            # parsed tree, properties, inline runs
build/render_png <file.md> out.png [w] [scale]
build/render_app <file.md> out.png [w] [h] [scale]   # window incl. sidebar
```

`render_png` at several scales is the cheapest regression test there is: layout
is scale-invariant by design, so the content height must not move.
