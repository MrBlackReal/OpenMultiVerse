# ============================================================
# Makefile — verse universe simulator
# ============================================================
# Linux:   make
# Windows: mingw32-make  (MSYS2 / MinGW-w64)
#
# The Dear ImGui multiverse menu is built BY DEFAULT. It needs the extern/cimgui
# submodule and a C++ compiler:
#   git submodule update --init --recursive
#   make
# To build without it (no C++/cimgui/g++ needed — menu.c compiles to inert stubs):
#   make IMGUI=0
# Build intermediates (.o/.d) go under build/<flavour>/, mirroring the source
# tree, never next to the sources. Each flavour (imgui / noimgui) has its own
# directory, so toggling IMGUI no longer needs `make clean`: the two sets of
# objects, compiled against different struct layouts, can never be mixed.

CC      = gcc
CXX     = g++
TARGET  = verse

SRCDIR  = src

# Sources live in role-based subdirectories (see docs/ARCHITECTURE.md §2). The list is
# explicit rather than a `find`: it documents the layout, stays portable to
# MSYS2 make, and makes adding a new layer a deliberate one-line edit.
#   core/   state, units, data model      sim/    physics, collision, lifecycle
#   field/  CosmicField/Radiance/graph    render/ GL pipeline
#   fx/     particle systems              ui/     menus, HUD, build mode
#   util/   audio, profiler, benchmark
SRCDIRS = $(SRCDIR) \
          $(SRCDIR)/core $(SRCDIR)/sim $(SRCDIR)/field \
          $(SRCDIR)/render $(SRCDIR)/fx $(SRCDIR)/ui $(SRCDIR)/util

SRCS    = $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.c))

# Every source directory is on the include path, so headers keep being included
# by bare name (`#include "body.h"`) regardless of which layer they live in.
INCS    = $(addprefix -I,$(SRCDIRS)) -Iextern/stb

# -MMD -MP emits a .d file per object listing the headers it #includes, so
# editing a header (e.g. src/core/body.h) forces every dependent .c to recompile.
# Without this, make's default rule only tracks the .c -> .o edge and a struct
# layout change in a header silently leaves stale objects compiled against the
# old layout — a memory-corruption / infinite-loop footgun.
CFLAGS  = -Wall -Wextra -O2 -std=c99 $(INCS) -fopenmp -MMD -MP

IMGUI      ?= 1
CIMGUI_DIR  = extern/cimgui
LINK        = $(CC)

BUILDROOT  ?= build
FLAVOUR     = $(if $(filter 1,$(IMGUI)),imgui,noimgui)
BUILDDIR    = $(BUILDROOT)/$(FLAVOUR)
OBJS        = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))

# Auto-detect platform
UNAME := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(UNAME), Linux)
    SDL_CFLAGS = $(shell sdl2-config --cflags)
    CFLAGS  += $(SDL_CFLAGS)
    LDFLAGS  = $(shell sdl2-config --libs) -lSDL2_ttf -lSDL2_mixer -lGL -lGLEW -lm -fopenmp
    EXT      =

else ifeq ($(UNAME), Darwin)
    SDL_CFLAGS = $(shell sdl2-config --cflags)
    CFLAGS  += $(SDL_CFLAGS)
    LDFLAGS  = $(shell sdl2-config --libs) -lSDL2_ttf -lSDL2_mixer -lGLEW \
               -framework OpenGL -lm -fopenmp
    EXT      =

else
    # Windows — MSYS2 / MinGW-w64
    # Set SDL2_DIR if SDL2 is not in the default MinGW prefix.
    # Example: SDL2_DIR = C:/msys64/mingw64
    SDL2_DIR ?= C:/msys64/mingw64
    SDL_CFLAGS = -I$(SDL2_DIR)/include/SDL2 -I$(SDL2_DIR)/include
    CFLAGS  += $(SDL_CFLAGS)
    LDFLAGS  = -L$(SDL2_DIR)/lib \
               -lSDL2 -lSDL2_ttf -lSDL2_mixer \
               -lglew32 \
               -lopengl32 -lglu32 \
               -lm -fopenmp -mwindows
    EXT      = .exe
    RC       = windres
    RC_OBJ   = $(BUILDDIR)/resource.o
endif

# ---- Optional Dear ImGui (cimgui) -----------------------------------
ifeq ($(IMGUI),1)
    # The submodule is now required for a default build, so fail with something
    # actionable instead of a hundred "cimgui.h: No such file" errors.
    ifeq ($(wildcard $(CIMGUI_DIR)/cimgui.h),)
        $(error extern/cimgui is missing. Run: git submodule update --init --recursive \
                — or build without the menu: make IMGUI=0)
    endif

    # C side: expose USE_IMGUI + the cimgui C API headers to menu.c.
    CFLAGS += -DUSE_IMGUI -DCIMGUI_DEFINE_ENUMS_AND_STRUCTS \
              -DCIMGUI_USE_SDL2 -DCIMGUI_USE_OPENGL3 \
              -I$(CIMGUI_DIR) -I$(CIMGUI_DIR)/generator/output

    # C++ side: cimgui + Dear ImGui + SDL2/OpenGL3 backends. The IMGUI_IMPL_API
    # define is what gives the backend functions C linkage (per cimgui docs).
    IMGUI_CXXFLAGS = -O2 -fno-exceptions -fno-rtti -std=c++11 \
                     -I$(CIMGUI_DIR) -I$(CIMGUI_DIR)/generator/output \
                     -I$(CIMGUI_DIR)/imgui -I$(CIMGUI_DIR)/imgui/backends \
                     -DIMGUI_USER_CONFIG=\"../cimconfig.h\" \
                     -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS=1 \
                     -DIMGUI_IMPL_API='extern "C" ' \
                     -DCIMGUI_USE_SDL2 -DCIMGUI_USE_OPENGL3 \
                     $(SDL_CFLAGS)

    CIMGUI_OBJS = $(addprefix $(BUILDDIR)/$(CIMGUI_DIR)/, \
                  cimgui.o \
                  cimgui_impl.o \
                  imgui/imgui.o \
                  imgui/imgui_draw.o \
                  imgui/imgui_tables.o \
                  imgui/imgui_widgets.o \
                  imgui/imgui_demo.o \
                  imgui/backends/imgui_impl_sdl2.o \
                  imgui/backends/imgui_impl_opengl3.o)

    OBJS    += $(CIMGUI_OBJS)
    LINK     = $(CXX)
    LDFLAGS += -lstdc++
endif

# ---- Rules ---------------------------------------------------------
all: $(TARGET)$(EXT)

# The binary is shared by both flavours, so switching IMGUI must relink even
# when the other flavour's objects are older than it. The stamp is rewritten
# only when the flavour actually changes, so it costs nothing otherwise.
FLAVOUR_STAMP = $(BUILDROOT)/.flavour

$(TARGET)$(EXT): $(OBJS) $(RC_OBJ) $(FLAVOUR_STAMP)
	$(LINK) -o $@ $(OBJS) $(RC_OBJ) $(LDFLAGS)

$(FLAVOUR_STAMP): FORCE
	@mkdir -p $(BUILDROOT)
	@echo $(FLAVOUR) | cmp -s - $@ || echo $(FLAVOUR) > $@

FORCE:

# Offline catalog converter — standalone, no SDL/OpenGL. Shares src/core/catalog.c
# with the simulator.
catalogtool$(EXT): tools/catalogtool.c $(SRCDIR)/core/catalog.c
	$(CC) -Wall -Wextra -O2 -std=c99 $(INCS) -o $@ $^ -lm

# On Windows the real target is catalogtool.exe, so a plain `make catalogtool`
# would fail with "No rule to make target". Alias it. (Guarded by EXT being
# non-empty: on Linux the alias and the real target are the same name.)
ifneq ($(EXT),)
.PHONY: catalogtool
catalogtool: catalogtool$(EXT)
endif

$(BUILDDIR)/resource.o: resource.rc
	@mkdir -p $(@D)
	$(RC) resource.rc -O coff -o $@

# `%` spans '/', so this one rule covers src/main.c and every src/<layer>/*.c,
# each landing at build/<flavour>/src/<layer>/<name>.o (+ its .d).
$(BUILDDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

# Dear ImGui / cimgui C++ translation units (only built when IMGUI=1).
$(BUILDDIR)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(IMGUI_CXXFLAGS) -c -o $@ $<

# CI guard (see .github/workflows/ci.yml): print the sources make will actually
# compile, so a new layer directory under src/ that was never added to SRCDIRS
# is caught instead of silently not being built.
print-srcs:
	@echo $(SRCS)

clean:
	rm -rf $(BUILDROOT)
	rm -f $(TARGET) $(TARGET).exe catalogtool catalogtool.exe
	@# Leftovers from the old in-tree layout (objects beside the sources).
	rm -f $(foreach d,$(SRCDIRS),$(d)/*.o $(d)/*.d) resource.o
	rm -f $(CIMGUI_DIR)/*.o $(CIMGUI_DIR)/*.d $(CIMGUI_DIR)/imgui/*.o \
	      $(CIMGUI_DIR)/imgui/*.d $(CIMGUI_DIR)/imgui/backends/*.o \
	      $(CIMGUI_DIR)/imgui/backends/*.d

# Pull in the auto-generated header dependencies (.d files from -MMD). The leading
# '-' suppresses errors on the first build before any .d files exist.
-include $(OBJS:.o=.d)

.PHONY: all clean print-srcs FORCE
