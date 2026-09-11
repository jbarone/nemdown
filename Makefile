# nemdown — native Wayland markdown viewer.
#
# Usage:
#   make            build the debug binary (default)
#   make release    optimised build
#   make asan       address + UB sanitizers
#   make run FILE=x.md
#   make install    install to $(PREFIX), default /usr/local
#   make clean
#
# Generated Wayland protocol code lands in build/proto/ and is never committed.

.DEFAULT_GOAL := debug

PREFIX  ?= /usr/local
DESTDIR ?=
MODE    ?= debug

# CC is a Make built-in with a default value, so `?=` would never override it.
ifeq ($(origin CC),default)
  CC := clang
endif

PKGS := wayland-client xkbcommon cairo pangocairo md4c yaml-0.1 \
        gdk-pixbuf-2.0 librsvg-2.0

ifneq ($(shell pkg-config --exists $(PKGS) && echo ok),ok)
  $(error missing dependencies: run `pkg-config --print-errors $(PKGS)`)
endif

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS)) -lm

WL_SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner)
WL_PROTO   := $(shell pkg-config --variable=pkgdatadir wayland-protocols)

# tablet-v2 is not a feature we use: cursor-shape-v1's generated code references
# zwp_tablet_tool_v2_interface, so omitting it is an undefined symbol at link time.
PROTOCOLS := \
  $(WL_PROTO)/stable/xdg-shell/xdg-shell.xml \
  $(WL_PROTO)/stable/viewporter/viewporter.xml \
  $(WL_PROTO)/stable/tablet/tablet-v2.xml \
  $(WL_PROTO)/staging/fractional-scale/fractional-scale-v1.xml \
  $(WL_PROTO)/staging/cursor-shape/cursor-shape-v1.xml \
  $(WL_PROTO)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml

GEN   := build/proto
BUILD := build/$(MODE)

# Resolve each protocol XML by basename, wherever in the tree it lives.
vpath %.xml $(sort $(dir $(PROTOCOLS)))

PROTO_BASE := $(basename $(notdir $(PROTOCOLS)))
PROTO_HDR  := $(addprefix $(GEN)/,$(addsuffix -client-protocol.h,$(PROTO_BASE)))
PROTO_SRC  := $(addprefix $(GEN)/,$(addsuffix -protocol.c,$(PROTO_BASE)))

SRC := $(shell find src -name '*.c' 2>/dev/null | sort)
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC)) \
       $(patsubst $(GEN)/%.c,$(BUILD)/proto/%.o,$(PROTO_SRC))
DEP := $(OBJ:.o=.d)

# -std=c11 keeps the language strict ISO; _GNU_SOURCE opens the glibc surface we
# genuinely need (memfd_create, timerfd_create, inotify_init1, ppoll).
STD  := -std=c11 -D_GNU_SOURCE
WARN := -Wall -Wextra -Wpedantic -Wshadow -Wvla -Wpointer-arith \
        -Wwrite-strings -Wstrict-prototypes -Wmissing-prototypes \
        -Wold-style-definition -Wno-unused-parameter

CFLAGS_debug   := -Og -g3 -fno-omit-frame-pointer -DNEMDOWN_DEBUG=1
CFLAGS_release := -O2 -g -DNDEBUG -D_FORTIFY_SOURCE=3 -fstack-protector-strong
CFLAGS_asan    := -O1 -g3 -fno-omit-frame-pointer -DNEMDOWN_DEBUG=1 \
                  -fsanitize=address,undefined -fno-sanitize-recover=all

LDFLAGS_debug   :=
LDFLAGS_release := -Wl,-O1 -Wl,--as-needed
LDFLAGS_asan    := -fsanitize=address,undefined

CFLAGS  = $(STD) $(WARN) $(CFLAGS_$(MODE)) -Isrc -I$(GEN) $(PKG_CFLAGS) -MMD -MP
LDFLAGS = $(LDFLAGS_$(MODE))

debug release asan:
	@$(MAKE) --no-print-directory MODE=$@ build/$@/nemdown

$(GEN)/%-client-protocol.h: %.xml
	@mkdir -p $(@D)
	$(WL_SCANNER) -s client-header $< $@

$(GEN)/%-protocol.c: %.xml
	@mkdir -p $(@D)
	$(WL_SCANNER) -s private-code $< $@

# Every object needs every generated header present first, or a parallel build
# races the scanner. Coarse, correct, and cheap.
$(OBJ): $(PROTO_HDR)

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# Generated protocol code is machine output; we do not lint it.
$(BUILD)/proto/%.o: $(GEN)/%.c
	@mkdir -p $(@D)
	$(CC) $(STD) -O2 -g -I$(GEN) $(PKG_CFLAGS) -c -o $@ $<

$(BUILD)/nemdown: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(PKG_LIBS)

# Test harnesses link the engine only: no Wayland, so they run headless.
ENGINE_SRC := $(wildcard src/doc/*.c) src/util/log.c

build/dump_md4c: test/dump_md4c.c
	@mkdir -p build
	$(CC) $(STD) $(WARN) -O0 -g -Isrc $(PKG_CFLAGS) -o $@ $< $(PKG_LIBS)

build/dump_tree: test/dump_tree.c $(ENGINE_SRC)
	@mkdir -p build
	$(CC) $(STD) $(WARN) -O0 -g -Isrc $(PKG_CFLAGS) -o $@ $^ $(PKG_LIBS)

tools: build/dump_md4c build/dump_tree

test: tools
	@bash test/run.sh

run: debug
	./build/debug/nemdown $(FILE)

install: release
	install -Dm755 build/release/nemdown $(DESTDIR)$(PREFIX)/bin/nemdown
	install -Dm644 nemdown.desktop $(DESTDIR)$(PREFIX)/share/applications/nemdown.desktop
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/nemdown/README.md

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nemdown \
	      $(DESTDIR)$(PREFIX)/share/applications/nemdown.desktop
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/nemdown

clean:
	rm -rf build compile_commands.json

# bear is not installed and vendoring it would be exactly the machinery the
# project avoids, so clangd's database is generated from our own flags.
# Note: no JSON escaping. Fine while no flag contains a quote or backslash.
compile_commands.json: Makefile
	@printf '[\n' > $@
	@i=0; n=$$(echo $(SRC) $(PROTO_SRC) | wc -w); \
	for f in $(SRC) $(PROTO_SRC); do \
	  i=$$((i+1)); \
	  printf '  {"directory": "%s", "file": "%s", "command": "%s %s -c %s"}' \
	    "$(CURDIR)" "$(CURDIR)/$$f" "$(CC)" "$(CFLAGS)" "$$f" >> $@; \
	  [ $$i -lt $$n ] && printf ',' >> $@; printf '\n' >> $@; \
	done
	@printf ']\n' >> $@

.PHONY: debug release asan run install uninstall clean tools test

-include $(DEP)
