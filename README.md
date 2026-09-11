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
j / k / arrows     scroll a line       g / G          top / bottom
{ / }              previous/next para  f              reading guide on/off
h / l              pan a code fence    Backspace      back
Ctrl-d / Ctrl-u    half page           PgUp / PgDn    page
/ then n / N       search, next/prev   b              toggle the sidebar
+ / - / 0          zoom in/out/reset   r              reload
Ctrl-C             copy                q / Esc        quit
```

A thin teal mark in the left margin tracks the paragraph you are on, moving
as you scroll and stepping a whole paragraph at a time with `{` and `}`. It is
on by default; `f` turns it off.

Clicking a contents entry scrolls to that heading. Clicking a `[[wikilink]]`
opens that note — `Backspace` goes back. Clicking a link opens it;
clicking a task checkbox toggles it **in the file**. Callouts fold and unfold
from the chevron in their title row. Hovering a code fence
reveals a copy button in its top-right corner. Code does not wrap. Pan an overflowing fence
with `h`/`l`, by scrolling sideways, or with Shift and a wheel; a fade at the
edge shows there is more. The keys act on the visible fence nearest the middle
of the window. Drag to select text,
double-click for a word, triple-click for a block. Drag the divider to resize
the sidebar.

The sidebar's two panes scroll independently — the wheel goes to whichever one
the pointer is over. Overlay scrollbars appear while scrolling and fade out.

Esc closes the search field, then clears a selection, then quits.

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
inline and fenced code with syntax highlighting, `==highlights==`, links and `[[wikilinks]]`
(including `[[note|shown as this]]` and `[[note#heading]]`), nested and ordered
lists, task checkboxes, blockquotes,
Obsidian callouts (`> [!warning] Title`), tables with alignment, horizontal
rules, YAML frontmatter, and `$math$` styled but not typeset — display `$$math$$` gets its own labelled
panel. Images carry their alt text as a caption.

Images are decoded (PNG/JPEG/GIF/BMP/TIFF via gdk-pixbuf, SVG via librsvg),
including Obsidian's `![[img.png|400]]` size hints. Missing, unreadable and
remote images draw a placeholder — nothing is fetched over the network.

Wikilinks resolve next to the document that mentions them — `[[note]]` finds
`note.md` in the same directory. This is not a vault index: there is no
directory walk, and a link that does not resolve is rendered muted and is not
clickable.

Not yet: LaTeX typesetting, footnotes, mermaid, a vault browser, WebP.

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
