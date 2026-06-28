/*
 * menu.c — Dear ImGui (cimgui) universe picker + live-laws overlay.
 *
 * Active only when built with USE_IMGUI (make IMGUI=1). Otherwise the functions
 * below are no-op stubs so the rest of the program is unaffected.
 */
#include "menu.h"
#include "presets.h"
#include "laws.h"
#include "catalog.h"
#include "universe.h"

#ifdef USE_IMGUI

/* Real-data catalogs offered by the in-app importer. Importing converts the
 * catalog to a cache universe and loads it. */
#define IMPORT_CACHE_PATH "assets/universes/_imported.json"
typedef struct { const char *label; const char *path; int type; } CatalogEntry;
static const CatalogEntry s_catalogs[] = {
    { "Exoplanets — TRAPPIST-1",       "assets/catalogs/trappist1.csv",         CATALOG_EXOPLANETS },
    { "Exoplanets — neighborhood",     "assets/catalogs/exoplanets_sample.csv", CATALOG_EXOPLANETS },
    { "Horizons — Solar System",       "assets/catalogs/horizons_sample.csv",   CATALOG_HORIZONS   },
    { "Gaia — nearby stars",           "assets/catalogs/gaia_sample.csv",       CATALOG_GAIA       },
};

/* cimgui: ask for the C-friendly struct/enum definitions and the SDL2+OpenGL3
 * backend declarations. These defines must precede the cimgui includes; guard
 * them so the Makefile may also pass them on the command line without warnings. */
#ifndef CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#endif
#ifndef CIMGUI_USE_SDL2
#define CIMGUI_USE_SDL2
#endif
#ifndef CIMGUI_USE_OPENGL3
#define CIMGUI_USE_OPENGL3
#endif
#include "cimgui.h"
#include "cimgui_impl.h"

static ImGuiContext *s_ctx     = NULL;
static int           s_visible = 0;

void menu_init(SDL_Window *win, SDL_GLContext gl)
{
    if (s_ctx) return;
    s_ctx = igCreateContext(NULL);
    igStyleColorsDark(NULL);
    ImGui_ImplSDL2_InitForOpenGL(win, (void *)gl);
    ImGui_ImplOpenGL3_Init("#version 130");
}

void menu_shutdown(void)
{
    if (!s_ctx) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    igDestroyContext(s_ctx);
    s_ctx = NULL;
}

int menu_process_event(const SDL_Event *e)
{
    if (!s_ctx) return 0;
    ImGui_ImplSDL2_ProcessEvent(e);
    if (!s_visible) return 0;
    ImGuiIO *io = igGetIO_Nil();
    return (io->WantCaptureMouse || io->WantCaptureKeyboard) ? 1 : 0;
}

void menu_set_visible(int visible) { s_visible = visible ? 1 : 0; }
int  menu_visible(void)            { return s_visible; }
void menu_toggle(void)             { s_visible = !s_visible; }

int menu_render(int current_preset, int *laws_changed, const char **out_load_path)
{
    int switch_to = -1;
    if (laws_changed) *laws_changed = 0;
    if (!s_ctx) return -1;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    igNewFrame();

    if (s_visible) {
        igSetNextWindowPos((ImVec2_c){ 40.0f, 40.0f }, ImGuiCond_FirstUseEver,
                           (ImVec2_c){ 0.0f, 0.0f });
        igSetNextWindowSize((ImVec2_c){ 440.0f, 0.0f }, ImGuiCond_FirstUseEver);

        bool open = s_visible ? true : false;   /* window close [x] -> open=false */
        if (igBegin("Multiverse", &open, ImGuiWindowFlags_NoCollapse)) {
            igText("Choose a universe:");
            igSeparator();
            for (int i = 0; i < preset_count(); i++) {
                const UniversePreset *p = preset_at(i);
                if (!p) continue;
                if (igSelectable_Bool(p->name, i == current_preset, 0,
                                      (ImVec2_c){ 0.0f, 0.0f }))
                    switch_to = i;
                if (p->blurb && p->blurb[0])
                    igTextWrapped("    %s", p->blurb);
            }

            igSpacing();
            igSeparator();
            igText("Live laws (edit the active universe):");

            /* Edit copies in human-friendly units, then write back to g_laws. */
            float g_ratio = (float)(g_laws.G / LAWS_DEFAULT_G);
            float fexp    = (float)g_laws.force_exp;
            float tscale  = (float)g_laws.time_scale;
            float lam_e15 = (float)(g_laws.lambda * 1.0e15);
            float pn      = (float)g_laws.pn_factor;

            int ch = 0;
            ch |= igSliderFloat("Gravity (xG)",   &g_ratio, 0.1f, 10.0f,  "%.2f",
                                ImGuiSliderFlags_Logarithmic);
            ch |= igSliderFloat("Force exponent", &fexp,    1.0f, 4.0f,   "%.2f", 0);
            ch |= igSliderFloat("Time scale",     &tscale,  0.1f, 20.0f,  "%.2f",
                                ImGuiSliderFlags_Logarithmic);
            ch |= igSliderFloat("Lambda (e-15)",  &lam_e15, -20.0f, 20.0f, "%.2f", 0);
            ch |= igSliderFloat("PN factor",      &pn,      0.0f, 5.0e6f, "%.0f", 0);

            if (ch) {
                g_laws.G          = (double)g_ratio * LAWS_DEFAULT_G;
                g_laws.force_exp  = (double)fexp;
                g_laws.time_scale = (double)tscale;
                g_laws.lambda     = (double)lam_e15 * 1.0e-15;
                g_laws.pn_factor  = (double)pn;
                if (laws_changed) *laws_changed = 1;
            }

            igSpacing();
            igSeparator();
            igText("Import real astronomical data:");
            for (int i = 0; i < (int)(sizeof(s_catalogs)/sizeof(s_catalogs[0])); i++) {
                if (igButton(s_catalogs[i].label, (ImVec2_c){ -1.0f, 0.0f })) {
                    int n = catalog_convert((CatalogType)s_catalogs[i].type,
                                            s_catalogs[i].path, IMPORT_CACHE_PATH, 0);
                    if (n >= 0 && out_load_path) *out_load_path = IMPORT_CACHE_PATH;
                }
            }

            igSpacing();
            igSeparator();
            igText("Save / load universe (live snapshot):");
            static char s_file[256] = "assets/universes/mysave.json";
            igInputText("file", s_file, sizeof(s_file), 0, NULL, NULL);
            if (igButton("Save snapshot", (ImVec2_c){ -1.0f, 0.0f }))
                universe_save(s_file);
            if (igButton("Load file", (ImVec2_c){ -1.0f, 0.0f }) && out_load_path)
                *out_load_path = s_file;

            igSpacing();
            igTextWrapped("Selecting a universe reloads its bodies and laws. "
                          "Slider edits change only the live universe. Save writes "
                          "the current live state; loading it restores that instant.");
        }
        igEnd();
        s_visible = open ? 1 : 0;   /* honour the window's close button */
    }

    igRender();
    ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
    return switch_to;
}

#else  /* ---- USE_IMGUI not defined: inert stubs ---- */

void menu_init(SDL_Window *win, SDL_GLContext gl) { (void)win; (void)gl; }
void menu_shutdown(void) {}
int  menu_process_event(const SDL_Event *e) { (void)e; return 0; }
void menu_set_visible(int visible) { (void)visible; }
int  menu_visible(void) { return 0; }
void menu_toggle(void) {}
int  menu_render(int current_preset, int *laws_changed, const char **out_load_path)
{
    (void)current_preset; (void)out_load_path;
    if (laws_changed) *laws_changed = 0;
    return -1;
}

#endif /* USE_IMGUI */
