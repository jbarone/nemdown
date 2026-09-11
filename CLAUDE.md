# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

nemdown is a native Wayland markdown viewer for Joshua's Hyprland setup, written
in C against raw `libwayland-client` — no GTK, no Qt, no browser engine. It
opens one file (`nemdown file.md`), renders it with fidelity aimed at Obsidian's
reading view, and shows a sidebar with the file's frontmatter properties above
its table of contents.

It is a sibling to `nemesis` (the Arch + Hyprland setup repo) and follows that
repo's conventions. Where nemesis has no precedent — a C build system, tests —
the convention is being established here.

## Intended layout

```
Makefile          hand-written; wayland-scanner codegen, debug/release/asan
src/main.c        argv and usage
src/app.c         app state, scrolling, dirty flags, action dispatch
src/wl/           the Wayland shell: connection, window, buffers, input
src/ui/           theme, typography, sidebar — the chrome around the document
src/ui/filechooser.c  the desktop's Open dialog, via xdg-desktop-portal
src/doc/          the document engine; doc.h is the ONLY header the shell uses
src/doc/mermaid*.c    mermaid diagrams; graphviz for graphs, by hand for sequences
test/             headless harnesses, assertion checks, fixtures; no compositor
tools/            lsan.supp, valgrind.supp
```

## Commands

- Build and run: `make` then `./build/debug/nemdown file.md`, or `make run FILE=x.md`.
- Headless engine checks: `make test`. It runs the two assertion harnesses
  first — `build/check_utf8` and `build/check_pathguard` — then parses every
  fixture. The fixture pass only reports; those two actually fail the build,
  because a regression in either is a hang or a path escape.
- Memory: `make asan`, then run with `LSAN_OPTIONS=suppressions=$PWD/tools/lsan.supp`.
  Without the suppressions, fontconfig's process-lifetime caches bury the signal.
- Also `make valgrind`. It complements the sanitizers rather than duplicating
  them — no recompilation, and it catches uninitialised reads ASan does not.
  Note librsvg's Rust runtime reports "possibly lost" interior pointers that
  are not ours, which is why the target narrows `--errors-for-leak-kinds`.
- Look at rendering without a compositor: `build/render_app file.md out.png 1100 900`.
- Inspect parser behaviour before trusting it: `build/dump_md4c` and `build/dump_tree`.
- Protocol debugging: `WAYLAND_DEBUG=1 ./build/debug/nemdown file.md`.

## Math

`$x^2$` and `$$...$$` are typeset, not styled source. There is no browser
engine and no LaTeX process: fonts with an **OpenType MATH table** carry the
constants TeX keeps in its font metrics, HarfBuzz exposes them through
`hb_ot_math_*`, and HarfBuzz is already linked via pangocairo. The pipeline
follows TeX's own — `math_parse.c` (source to atoms), `math_box.c` (atoms to
positioned boxes, TeXbook Appendix G), `math_paint.c` (boxes to glyphs), with
`math_syms.c` as the name-to-codepoint table.

Needs a math font: STIX Two Math (`otf-stix`, AUR) is preferred, Latin Modern
Math (`otf-latinmodern-math`, extra) and several others are accepted, and with
none installed math falls back to the styled-source panel it used to be. **Never
hard-code a measurement here** — read it from the face. STIX reports 70%/55%
script scaling where Noto reports 60%/50%.

## Mermaid

A ```` ```mermaid ```` fence is drawn, for four dialects. The split between the
two renderers is not arbitrary. `flowchart`, `stateDiagram-v2` and
`classDiagram` are GRAPHS — nodes and edges with no inherent geometry, so
something has to solve a layout — and that something is graphviz:
`mermaid_dot.c` translates each dialect to themed dot, `mermaid_gv.c` lays it
out and rasterises it. `sequenceDiagram` is not a graph at all: its columns and
rows are decided by the source and the only unknown is text width, which Pango
already answers, so `mermaid_seq.c` measures and draws it directly and needs no
dependency.

graphviz is **dlopened, not linked** (`mermaid_gv.c`, eight opaque entry
points). One binary works either way, and the package lists it as `optdepends`
— the same bargain the maths typesetter strikes with its font. Sequence
diagrams always draw; the graph dialects fall back to the code panel, whose
language label then says why.

The fence body is untrusted input that becomes DOT SOURCE, so a label escaping
its quoting would change the graph rather than merely look wrong. Every label
leaves through exactly one of `nd_mm_dot_str` (quoted attribute) or
`nd_mm_html_esc` (HTML-like label), and both drop control bytes. That is what
`test/check_mermaid.c` asserts, over mutated fences: whatever goes in, the dot
that comes out still has balanced quotes and braces. **If you change a parser,
run `make test` and then break it on purpose** — four mutations were checked
when it was written, and its first run found a real bug.

## Architecture

The shell (`src/wl`, `src/ui`) and the engine (`src/doc`) meet only at
`src/doc/doc.h`. The engine never touches Wayland, never learns the window size,
never owns a scroll offset and never sees an input event — it is handed a
viewport width and a document-space y, and owns the readable-measure clamp
itself. The shell never parses markdown. This is what lets the engine be driven
by a PNG harness with no display attached.

**Every pointer returned from `doc.h` is borrowed and dies on reload.** After
`nd_doc_reload` the caller must re-fetch the TOC and properties.

## Conventions

- **Catppuccin Mocha is the only theme**, hard-coded in `src/ui/theme.h`. Teal
  `#94e2d5` is the accent. There is no switcher and no template pipeline; do not
  add one, and do not ask about supporting other themes.
- Two-space indentation, no tabs. LF, final newline, no trailing whitespace.
- `-std=c11` with `-D_GNU_SOURCE`; clang is the compiler.
- Commits are atomic, with a prose body explaining *why*.

## Security posture

Markdown opened here may not have been written by the person reading it, so the
document body, its frontmatter, every path and URL it names, and the contents
of any image it references are all untrusted input. Three rules follow:

- **Document-supplied paths are confined to the tree** the first document was
  opened from (`src/doc/pathguard.c`, via `realpath`). Wikilinks and image
  sources that escape it resolve as missing. The containment root is fixed at
  open and never widened by navigation, or one hop would hand the next document
  the whole filesystem.
- **Only http, https and mailto links are opened.** xdg-open dispatches any
  `scheme:` to a registered handler and treats a bare path as a file, so
  anything else is refused rather than handed to the desktop.
- **Bytes are scrubbed to well-formed UTF-8 at load** (`src/util/utf8.c`). The
  scrub is length-preserving on purpose: the checkbox write indexes the real
  file by byte, so a replacement that changed length would retarget it.

The scrub and the guard are the two functions here with the least margin for
error, so neither is trusted to review alone: `test/check_utf8.c` sweeps
26.8M inputs differentially against `g_utf8_validate`, and
`test/check_pathguard.c` runs the escapes against a real symlink tree it
builds and tears down. **If you change either function, run `make test` and
then break it on purpose to confirm the harness still fails** — five
mutations were checked when they were written, and a sweep that cannot fail
is worth nothing.

**The compositor is the other untrusted input**, and a separate one. It is a
protocol peer, free to send any event in any order with any argument, and every
number it sends arrives as a size, a length or an index. An audit found the same
mistake in four places at once — a keymap size believed over the fd it described
(an uncatchable SIGBUS), configure dimensions and a fractional scale used
unclamped for buffer allocation, and a pool size narrowed to `int32_t` where
3.6GB reads as negative and 4GB as exactly zero. Clamp on the way in. Hyprland
is friendly; nothing here may depend on that.

Two known gaps, both judged not worth the cost so far. Filenames are not
scrubbed, so invalid bytes in a path reach `xdg_toplevel_set_title`; nothing
unscrubbed reaches Pango, because `nd_sidebar_paint` ignores its `title`
argument. And the guard checks a path that is opened afterwards by name, so
a symlink swapped in between is a race — the static attack, a symlink
committed to the tree, is caught, since `realpath` resolves it before the
comparison.

## Things that are easy to get wrong

These were each a real bug; the comments in the code say so at the site.

- **`CAIRO_HINT_METRICS_OFF`** on the shared Pango context is load-bearing. It
  makes layout scale-invariant, so a display-scale change is a repaint rather
  than a relayout, and hit-test geometry cannot drift. Content height must stay
  identical across `render_png` at 1.0/1.25/1.5/2.0.
- **Never multiply font sizes by the display scale.** The Cairo surface device
  scale already carries it.
- **md4c does not parse `![[x.png]]`** — it emits the whole thing as literal
  text. `preprocess.c` rewrites embeds to `![](<x>)` before parsing, skipping
  fenced and inline code.
- **Rewriting shifts byte offsets**, which breaks the task-mark offsets that
  make checkbox write-back possible. Use `nd_source_orig_offset`, never add
  `body_offset` directly.
- **Inline runs overlap and are sorted by nesting depth.** Do not flatten them
  into a style cross-product.
- **A tight list item has no `MD_BLOCK_P`**, so the paragraph is synthesised —
  and it must be synthesised on the first *inline event*, not the first text.
  `inl_begin()` resets the span stack, so a span opened before the leaf existed
  (`- **bold lead-in** rest`, or an item starting with a link) had its frame
  wiped and vanished on `leave_span`. The markers were consumed either way, so
  the emphasis disappeared while the text stayed — invisible in fixtures,
  obvious in any real document. `MD_SPAN_IMG` is the exception: it builds its
  own block, and forcing a leaf leaves an empty paragraph beside it.
- **Soft breaks fold to spaces**, so a paragraph's source line boundary is only
  recoverable via `nd_inline.first_break`. Callout titles depend on it.
- **`inotify` watches the parent directory, not the file**, because editors save
  by rename and a watch on the inode goes silent after the first save.
- **`prepare_read` must be matched by exactly one `read_events` or
  `cancel_read`** on every path, including `EINTR` and the flush retry.
- **cursor-shape-v1's generated code references `zwp_tablet_tool_v2_interface`**,
  so `tablet-v2.xml` must stay in the protocol list or the link fails.
- **`pango_layout_xy_to_index` writes the nearest index even when it returns
  FALSE.** Hit testing must bounds-check the point against the layout extents
  first, or clicks far to the right of a line activate the last link on it.
- **Pango's `trailing` counts characters, not bytes.** Advance with
  `g_utf8_next_char`; adding it to the index splits codepoints.
- **Selection and search hold block pointers and indices**, so both are cleared
  on reload and search is re-run after every layout.
- **gdk-pixbuf has no Cairo bridge** — the premultiplied ARGB32 conversion in
  `images.c` is hand-written because `gdk_cairo_surface_create_from_pixbuf`
  lives in GTK.
- **Wikilink anchors arrive as written (`#Some Heading`) but slugs are
  normalised**, so `nd_doc_anchor_y` slugifies its argument before matching.
- **Pango's valid-UTF-8 precondition is not advisory.** `pango_get_log_attrs()`
  loops forever on a buffer ending mid-sequence — one stray byte froze the
  whole viewer on a double-click. Hence the load-time scrub.
- **A silently-declined push must have its matching pop declined too.** This
  bit the block stack (an out-of-bounds write) and the span stack (vanishing
  links). Both count declines now.
- **Pango stops laying out above roughly 190KB in one layout** and reports a
  single line, so an enormous paragraph silently renders as a blank sliver.
  `nd_layout_for` caps the text it hands over so the failure is a visible
  truncation with a warning instead.
- **Anything per-match or per-block in a paint or search path must be culled to
  the viewport**, and positions resolved in one pass over the layout rather
  than per item — `pango_layout_index_to_line_x` and
  `pango_layout_get_line_readonly` both walk the line list from the start, so
  per-item lookup is quadratic. This was 79 seconds a frame.
- **A path the USER chose re-homes the containment root; a path a DOCUMENT
  names never does.** `nd_doc_open_chosen` exists for exactly that difference.
  Confining an Open-dialog choice to the previous document's tree would make
  its images silently fail to load; letting a wikilink widen the root would
  hand the next document the filesystem.
- **The file chooser runs on a worker thread and reports through a pipe**, not
  by embedding a GMainContext in the poll loop. The prepare_read dance is the
  most delicate thing in the program and a dialog is not worth risking it.
- **Copying yields rendered text, not markdown source.** md4c's text callback
  carries no source offset, so the mapping does not exist.
- **Atom classes are spacing, not bookkeeping.** The gap between two math
  atoms is a function of their classes, so `a+b` and `a=b` differ. TeXbook
  rule 5 — a Bin that cannot be binary becomes an Ord — is what sets the `-`
  in `-b` tight instead of as a subtraction.
- **Centring a delimiter on the maths axis is `(h-d)/2` MINUS the axis.**
  Adding it drops the delimiter by twice the axis height, which reads as
  parentheses sagging below the fraction they enclose.
- **Pango draws a shape attribute once per CHARACTER it covers.** An inline
  formula therefore hangs on the first character only, with the rest of its
  source under zero-width shapes — which keeps the LaTeX in the layout so
  search and copy still see text a person can type.
- **`$$...$$` is a SPAN extension, but block structure is decided first.** A
  lone `=` line inside a display formula is a setext heading underline and
  turns it into an H1. `preprocess.c` folds the newlines to spaces, which is
  length-preserving and so costs no edit record.
- **Mermaid output is `svg:cairo`, not `svg`.** Plain SVG writes text as
  text and lets the renderer shape it — and librsvg does not shape it the way
  graphviz measured it. graphviz measures through Pango WITH hinting at 96dpi,
  where Hack's 10.66px advance hints down to 10, then writes points; librsvg
  draws the same string unhinted at 13px and gets 4% more. Invisible on a node
  with a wide margin, obvious in a class-diagram cell, where the last member
  ran out through the right border. `svg:cairo` emits positioned glyph
  outlines, so measurement and drawing cannot disagree. The plain SVG is still
  rendered, but only for its viewBox: it states the size in POINTS, which is
  the coordinate system the dot's font sizes were written in.
- **A mermaid registry entry is a pointer that must not move.** The flowchart
  parser holds both ends of an edge while it parses the next node, so an array
  of structs that reallocs is a use-after-free — which is what it was, found by
  ASan. The registry stores individually allocated nodes for that reason.
- **graphviz leaks the virtual edges it builds for CLUSTER layout**, so a
  flowchart with a `subgraph` loses about a kilobyte per relayout. Not ours and
  nothing to free from here; suppressed narrowly by function name in both
  `tools/lsan.supp` and `tools/valgrind.supp` so other graphviz leaks still
  report.
- **`cairo_surface_set_device_scale` must come before the first draw.** Cairo
  does not return an error for a late one, it asserts and aborts.
- **Zoom scales a diagram, the display scale rasterises it.** They are separate
  knobs on the same object: `nd_mermaid_build` takes the font scale and bakes
  it into the drawn size, while the device scale is read off the Cairo target
  at paint time and only decides raster resolution. Confusing them gives either
  a diagram that ignores `+`/`-` or one that changes size when you move monitor.
- **`xkb_state_key_get_utf8` is snprintf-shaped.** It returns the bytes
  *required*, not the bytes written, so a key level carrying several keysyms
  reports more than it wrote. Bound the return against the local buffer, not
  only against wherever you are copying to — a bigger buffer does not fix it.
- **A clamp on the logical size is not a clamp on the allocation.** 16384
  logical at scale 1.5 is 24576 device and 2.4GB. The logical bound protects
  the scroll and zoom arithmetic; the device bound protects the buffer. They
  are separate limits and both are needed.
- **The buffer realloc test must include the scale.** Cairo's device scale is
  baked into the surface at creation, so 2000x1600@1.0 and 1000x800@2.0 have
  identical device dimensions — compare `buf_scale` too, or the document
  renders into a quarter of the window.
- **`render()` can run with a frame callback already outstanding**, because
  `xdg_surface.configure` must render once it has acked. Destroy the old
  callback before replacing it; otherwise `frame_done` destroys its own
  argument, NULLs the field, and the newer callback is stranded.
- **A deadline that only a frame callback can clear will latch
  `poll_timeout` at 0** if frames stop arriving — the 100%-CPU spin. Every
  timed effect must be self-clearing in `nd_app_tick`.
- **The clipboard payload goes through a memfd, not a pipe.** A pipe holds
  64KB and Ctrl-C with no selection copies the whole document, so the write
  blocked the loop — and a loop that is not running is not answering
  `xdg_wm_base.ping`, which is how Hyprland decides a client has hung.
- **Version clamping cuts both ways.** Binding above the advertised version
  kills the client, but libwayland does not check an opcode against the
  proxy's version either, so a request that postdates a *low* advertised
  version is an equally fatal protocol error with no message. `wl_compositor`
  below 4 is refused at connect because `damage_buffer` is unconditional.
