# nemdown

A native Wayland markdown viewer for Hyprland, written in C. No Electron, no
browser engine, no toolkit — `libwayland-client` + xdg-shell + `wl_shm`, with
Cairo and Pango doing the drawing.

![nemdown showing a document's frontmatter properties and table of contents in
the sidebar, with callouts, syntax-highlighted code, a table and typeset
mathematics in the reading pane](art/screenshot.png)

*Frontmatter properties and contents in the sidebar; Obsidian callouts,
syntax highlighting, tables and typeset LaTeX in the page. Catppuccin Mocha.*

```
nemdown file.md
```

The window is a split: a sidebar listing the file's frontmatter properties above
its table of contents, and the rendered document beside it. Catppuccin Mocha,
teal accent, no theme switcher.

## Installing

Arch, as a pacman-tracked package. Needs `base-devel`.

```
git clone https://github.com/jbarone/nemdown.git
cd nemdown/packaging/aur/nemdown-git && makepkg -si
```

Or, with an AUR helper, straight from the PKGBUILD directory:

```
yay -B nemdown/packaging/aur/nemdown-git
```

Either gives a real package: `pacman -Qi nemdown-git` knows it, `-Rns` removes
it cleanly, and rebuilding picks up new commits. It pulls its own build and
runtime dependencies, so there is no list to install by hand.

Three things are optional and none is installed for you, because each only
matters to documents that use it and each degrades to showing its source rather
than failing. Maths needs a font carrying an OpenType MATH table — `otf-stix`
preferred, `otf-latinmodern-math` also works. Mermaid flowcharts, state
diagrams and class diagrams need `graphviz` (sequence diagrams do not, and
always draw). `wl-clipboard` enables copying. All three are `optdepends`.

It registers a launcher entry and an icon, and claims `text/markdown`, so it
can be opened from a menu or by clicking a `.md` file. Launched with no file it
asks for one through the desktop's own Open dialog (`xdg-desktop-portal`); `o`
does the same from inside.

`packaging/aur/nemdown` is the same package built from a tagged release rather
than the tip; it needs a tag to exist first. Neither is on the AUR yet.

To build without installing, see [Building](#building).

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
o                  open a file                /         search
                                              n / N     next / previous match
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

For working on nemdown. To just use it, see [Installing](#installing) — the
package pulls these in for you.

```
make          # debug build at build/debug/nemdown
make release  # optimised
make asan     # address + UB sanitizers
make test     # assertion checks, then the engine harnesses over test/fixtures
make valgrind # memcheck over every fixture; catches what ASan does not
sudo make install
```

Build: `clang` `pkgconf` `wayland-protocols`. Link: `wayland` `libxkbcommon`
`cairo` `pango` `md4c` `libyaml` `gdk-pixbuf2` `librsvg` `glib2` `harfbuzz` —
glib for its UTF-8 helpers and harfbuzz for the OpenType MATH table, both
linked directly rather than only through pango. All from the official repos.
This list is the one in `packaging/aur/nemdown-git/PKGBUILD`, which a
clean-chroot build checks; if the two ever disagree, the PKGBUILD is right.

At runtime: `wl-clipboard` for copying, Hack Nerd Font and Hack Nerd Font Mono
for the typography, a maths font — `otf-stix` (AUR) preferred,
`otf-latinmodern-math` also works — and `graphviz` for mermaid's graph
dialects. graphviz is opened with `dlopen` rather than linked, so there is one
binary either way: installed, the diagrams draw; absent, those fences show
their source and everything else is unchanged.

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

### Mermaid

A ```` ```mermaid ```` fence is drawn rather than listed, for four dialects:

| dialect | drawn by | covers |
|---|---|---|
| `flowchart` / `graph` | graphviz | all the node shapes, `-->` `---` `-.->` `==>` `~~~`, both label forms, `&` chains, nested `subgraph` |
| `sequenceDiagram` | nemdown | `participant`/`actor` with `as`, `autonumber`, every arrow form, activations (`+`/`-` included), self-messages, notes, `alt`/`else`/`opt`/`loop`/`par`/`critical`/`break` |
| `stateDiagram-v2` | graphviz | `[*]` start and end, transitions with labels, descriptions, composite states, `<<choice>>`, `<<fork>>`/`<<join>>` |
| `classDiagram` | graphviz | members with visibility, static and abstract, `~T~` generics, `<<annotations>>`, every relationship with multiplicities and labels, `note for` |

There is no JavaScript anywhere in this: mermaid's own renderer is a browser
program, and running one would undo the point of the project. Three of the four
dialects are *graphs* — a set of nodes and edges with no inherent geometry — so
they are translated to dot and laid out by graphviz in-process, themed in
Catppuccin so the result sits in the document rather than on top of it. The
fourth is not a graph at all: a sequence diagram's columns and rows are already
decided by the source, and the only unknown is how wide the text is, which
Pango answers — so it is measured and drawn directly, and needs no dependency.

Two consequences worth stating plainly. The output looks like nemdown, not like
mermaid.js — the same diagram, drawn in the document's own hand. And what is
supported is these four dialects, not mermaid: a `gantt`, an `erDiagram` or a
`pie` shows its source, as does anything that fails to parse. A graph dialect
with graphviz missing says so on the fence label.

Not yet: footnotes, the other mermaid dialects, a vault browser, WebP.

## Development tools

Headless, so they need no compositor:

```
build/dump_md4c  <file.md>            # raw md4c callback stream
build/dump_tree  <file.md>            # parsed tree, properties, inline runs
build/render_png <file.md> out.png [w] [scale]
build/render_app <file.md> out.png [w] [h] [scale] [scroll]  # incl. sidebar
build/check_utf8                      # 26.8M-input sweep of the UTF-8 scrub
build/check_pathguard                 # path containment against a symlink tree
build/check_mermaid [rounds] [builds] # mutation sweep over the mermaid parsers
```

These three assert rather than report, and `make test` runs them first: a
regression in the first two is a hang or a path escape, and in the third it is
a document steering what gets handed to graphviz.

`render_png` at several scales is the cheapest regression test there is: layout
is scale-invariant by design, so the content height must not move.
