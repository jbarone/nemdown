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
src/doc/          the document engine; doc.h is the ONLY header the shell uses
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
- **Copying yields rendered text, not markdown source.** md4c's text callback
  carries no source offset, so the mapping does not exist.
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
