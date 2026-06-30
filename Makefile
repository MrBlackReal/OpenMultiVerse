# ============================================================
# Makefile — verse universe simulator
# ============================================================
# Linux:   make
# Windows: mingw32-make  (MSYS2 / MinGW-w64)
#
# Optional Dear ImGui multiverse menu (needs the extern/cimgui submodule):
#   git submodule update --init --recursive
#   make IMGUI=1
# Without IMGUI=1 the menu code compiles to inert stubs and no C++/cimgui is
# required — the default build is unchanged.

CC      = gcc
CXX     = g++
TARGET  = verse

SRCDIR  = src
SRCS    = $(wildcard $(SRCDIR)/*.c)
OBJS    = $(SRCS:.c=.o)

# -MMD -MP emits a .d file per object listing the headers it #includes, so
# editing a header (e.g. src/body.h) forces every dependent .c to recompile.
# Without this, make's default rule only tracks the .c -> .o edge and a struct
# layout change in a header silently leaves stale objects compiled against the
# old layout — a memory-corruption / infinite-loop footgun.
CFLAGS  = -Wall -Wextra -O2 -std=c99 -I$(SRCDIR) -fopenmp -MMD -MP

IMGUI      ?= 0
CIMGUI_DIR  = extern/cimgui
LINK        = $(CC)

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
    RC_OBJ   = resource.o
endif

# ---- Optional Dear ImGui (cimgui) -----------------------------------
ifeq ($(IMGUI),1)
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

    CIMGUI_OBJS = $(CIMGUI_DIR)/cimgui.o \
                  $(CIMGUI_DIR)/cimgui_impl.o \
                  $(CIMGUI_DIR)/imgui/imgui.o \
                  $(CIMGUI_DIR)/imgui/imgui_draw.o \
                  $(CIMGUI_DIR)/imgui/imgui_tables.o \
                  $(CIMGUI_DIR)/imgui/imgui_widgets.o \
                  $(CIMGUI_DIR)/imgui/imgui_demo.o \
                  $(CIMGUI_DIR)/imgui/backends/imgui_impl_sdl2.o \
                  $(CIMGUI_DIR)/imgui/backends/imgui_impl_opengl3.o

    OBJS    += $(CIMGUI_OBJS)
    LINK     = $(CXX)
    LDFLAGS += -lstdc++
endif

# ---- Rules ---------------------------------------------------------
all: $(TARGET)$(EXT)

$(TARGET)$(EXT): $(OBJS) $(RC_OBJ)
	$(LINK) -o $@ $^ $(LDFLAGS)

# Offline catalog converter — standalone, no SDL/OpenGL. Shares src/catalog.c
# with the simulator.
catalogtool$(EXT): tools/catalogtool.c $(SRCDIR)/catalog.c
	$(CC) -Wall -Wextra -O2 -std=c99 -I$(SRCDIR) -o $@ $^ -lm

resource.o: resource.rc
	$(RC) resource.rc -O coff -o resource.o

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Dear ImGui / cimgui C++ translation units (only built when IMGUI=1).
%.o: %.cpp
	$(CXX) $(IMGUI_CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(SRCDIR)/*.o $(SRCDIR)/*.d $(TARGET) $(TARGET).exe resource.o catalogtool catalogtool.exe
	rm -f $(CIMGUI_DIR)/*.o $(CIMGUI_DIR)/imgui/*.o $(CIMGUI_DIR)/imgui/backends/*.o

# Pull in the auto-generated header dependencies (.d files from -MMD). The leading
# '-' suppresses errors on the first build before any .d files exist.
-include $(OBJS:.o=.d)

.PHONY: all clean
