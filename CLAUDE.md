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
test/             headless harnesses and fixtures; no compositor required
tools/            lsan.supp
```

## Commands

- Build and run: `make` then `./build/debug/nemdown file.md`, or `make run FILE=x.md`.
- Headless engine checks: `make test`.
- Memory: `make asan`, then run with `LSAN_OPTIONS=suppressions=$PWD/tools/lsan.supp`.
  Without the suppressions, fontconfig's process-lifetime caches bury the signal.
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
- **Soft breaks fold to spaces**, so a paragraph's source line boundary is only
  recoverable via `nd_inline.first_break`. Callout titles depend on it.
- **`inotify` watches the parent directory, not the file**, because editors save
  by rename and a watch on the inode goes silent after the first save.
- **`prepare_read` must be matched by exactly one `read_events` or
  `cancel_read`** on every path, including `EINTR` and the flush retry.
- **cursor-shape-v1's generated code references `zwp_tablet_tool_v2_interface`**,
  so `tablet-v2.xml` must stay in the protocol list or the link fails.
