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

### Reading

```
j / k              previous / next block      { / }     same, always
g / G              first / last block         f         reading guide on/off
Home / End         first / last block
up / down          scroll a line              PgUp/PgDn page
Ctrl-u / Ctrl-d    half page
h / l              pan a code fence
```

### Document

```
/                  search                     n / N     next / previous match
b                  toggle the sidebar         r         reload from disk
+ / - / 0          zoom in / out / reset      Backspace back to the last note
Ctrl-C             copy                       q         quit
Esc                close search, then clear a selection, then quit
```

`+` also accepts `=` so it needs no shift, and `+ - 0` work on the numeric
keypad.

While the search field is open, typing edits the query, `Backspace` deletes a
character, `Enter` keeps the matches highlighted and closes the field, and
`Esc` cancels the search outright.

### Mouse

```
wheel              scroll the pane under the pointer
Shift-wheel        pan a code fence sideways
click              follow a link or [[wikilink]], toggle a task checkbox,
                   fold a callout, copy a fence, jump to a contents entry
drag               select text          double-click  a word
drag the divider   resize the sidebar   triple-click  a block
```

## The reading guide

A thin teal mark in the left margin tracks the block you are on. `j` and `k`
(or `{` and `}`) move it a block at a time and the page follows: it walks down
a still page until it reaches the middle, then the page scrolls to keep it
centred, and at the end of the document the page stops and it walks down to the
last block. `g` and `G` take it to the first and last block.

The arrows still scroll a line at a time without moving it, and scrolling by
hand moves it only as far as it must to stay on screen. It is on by default;
`f` turns it off — and with it off, `j`, `k`, `g` and `G` go back to scrolling
the page.

## Mouse, in detail

Clicking a contents entry scrolls to that heading. Clicking a `[[wikilink]]`
opens that note — `Backspace` goes back. Clicking a link opens it; clicking a
task checkbox toggles it **in the file**. Callouts fold and unfold from the
chevron in their title row. Hovering a code fence reveals a copy button in its
top-right corner.

Code does not wrap. Pan an overflowing fence with `h`/`l`, by scrolling
sideways, or with Shift and a wheel; a fade at the edge shows there is more.
The keys act on the visible fence nearest the middle of the window.

Drag to select text, double-click for a word, triple-click for a block. Drag
the divider to resize the sidebar.

The sidebar's two panes scroll independently — the wheel goes to whichever one
the pointer is over. Overlay scrollbars appear while scrolling and fade out.

Esc closes the search field, then clears a selection, then quits.

## Building

```
make          # debug build at build/debug/nemdown
make release  # optimised
make asan     # address + UB sanitizers
make test     # assertion checks, then the engine harnesses over test/fixtures
make valgrind # memcheck over every fixture; catches what ASan does not
sudo make install
```

Dependencies, all from the Arch repos: `wayland` `wayland-protocols` `cairo`
`pango` `md4c` `libyaml` `libxkbcommon` `gdk-pixbuf2` `librsvg`, plus
`wl-clipboard` at runtime. Fonts: Hack Nerd Font and Hack Nerd Font Mono, plus
a maths font for typesetting — `otf-stix` (AUR) is preferred,
`otf-latinmodern-math` also works, and without either, maths falls back to
source.

## Rendering

Aims at Obsidian's reading view. Supported: headings, emphasis and nesting,
inline and fenced code with syntax highlighting, `==highlights==`, links and
`[[wikilinks]]` (including `[[note|shown as this]]` and `[[note#heading]]`),
nested and ordered lists, task checkboxes, blockquotes, Obsidian callouts
(`> [!warning] Title`), tables with alignment, horizontal rules, YAML
frontmatter, and LaTeX maths — `$inline$` and `$$display$$` are
typeset, not shown as source. Images carry their alt text as a caption.

Images are decoded (PNG/JPEG/GIF/BMP/TIFF via gdk-pixbuf, SVG via librsvg),
including Obsidian's `![[img.png|400]]` size hints. Missing, unreadable and
remote images draw a placeholder — nothing is fetched over the network.

Wikilinks resolve next to the document that mentions them — `[[note]]` finds
`note.md` in the same directory. This is not a vault index: there is no
directory walk, and a link that does not resolve is rendered muted and is not
clickable.

Maths is typeset against the OpenType MATH table of an installed maths font —
no browser engine and no LaTeX process. Fractions, radicals, scripts, big
operators with limits, stretchy delimiters, matrices, `cases`, `align`,
accents, `\binom` and the `\mathbb`/`\mathcal` alphabets all work, with TeX's
atom-class spacing. Without a maths font installed it falls back to showing the
source in a labelled panel.

Not yet: footnotes, mermaid, a vault browser, WebP.

## Development tools

Headless, so they need no compositor:

```
build/dump_md4c  <file.md>            # raw md4c callback stream
build/dump_tree  <file.md>            # parsed tree, properties, inline runs
build/render_png <file.md> out.png [w] [scale]
build/render_app <file.md> out.png [w] [h] [scale] [scroll]  # incl. sidebar
build/check_utf8                      # 26.8M-input sweep of the UTF-8 scrub
build/check_pathguard                 # path containment against a symlink tree
```

`check_utf8` and `check_pathguard` assert rather than report, and `make test`
runs them first: a regression in either is a hang or a path escape.

`render_png` at several scales is the cheapest regression test there is: layout
is scale-invariant by design, so the content height must not move.
