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
#include "camera.h"
#include "body.h"
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

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

/* A calm indigo-cyan accent used throughout, so the panel reads as a product
 * UI rather than a raw debug overlay. */
static void menu_apply_style(void)
{
    ImGuiStyle *st = igGetStyle();
    st->WindowRounding     = 8.0f;
    st->ChildRounding      = 6.0f;
    st->FrameRounding      = 5.0f;
    st->GrabRounding       = 5.0f;
    st->PopupRounding      = 5.0f;
    st->ScrollbarRounding  = 6.0f;
    st->TabRounding        = 5.0f;
    st->WindowBorderSize   = 1.0f;
    st->FrameBorderSize    = 0.0f;
    st->WindowPadding      = (ImVec2_c){ 14.0f, 12.0f };
    st->FramePadding       = (ImVec2_c){ 10.0f, 6.0f };
    st->ItemSpacing        = (ImVec2_c){ 9.0f, 8.0f };
    st->ItemInnerSpacing   = (ImVec2_c){ 8.0f, 6.0f };
    st->GrabMinSize        = 11.0f;
    st->ScrollbarSize      = 12.0f;
    st->WindowTitleAlign   = (ImVec2_c){ 0.5f, 0.5f };

    ImVec4_c *c = st->Colors;
    const float a = 0.33f, b = 0.62f, d = 0.95f;   /* accent RGB */
    c[ImGuiCol_WindowBg]        = (ImVec4_c){ 0.09f, 0.10f, 0.12f, 0.96f };
    c[ImGuiCol_ChildBg]         = (ImVec4_c){ 0.12f, 0.13f, 0.16f, 0.55f };
    c[ImGuiCol_Border]          = (ImVec4_c){ a, b, d, 0.22f };
    c[ImGuiCol_TitleBg]         = (ImVec4_c){ 0.08f, 0.09f, 0.11f, 1.0f };
    c[ImGuiCol_TitleBgActive]   = (ImVec4_c){ a * 0.45f, b * 0.45f, d * 0.45f, 1.0f };
    c[ImGuiCol_Header]          = (ImVec4_c){ a, b, d, 0.30f };
    c[ImGuiCol_HeaderHovered]   = (ImVec4_c){ a, b, d, 0.55f };
    c[ImGuiCol_HeaderActive]    = (ImVec4_c){ a, b, d, 0.80f };
    c[ImGuiCol_Button]          = (ImVec4_c){ a, b, d, 0.28f };
    c[ImGuiCol_ButtonHovered]   = (ImVec4_c){ a, b, d, 0.55f };
    c[ImGuiCol_ButtonActive]    = (ImVec4_c){ a, b, d, 0.85f };
    c[ImGuiCol_FrameBg]         = (ImVec4_c){ 0.17f, 0.19f, 0.23f, 1.0f };
    c[ImGuiCol_FrameBgHovered]  = (ImVec4_c){ a, b, d, 0.25f };
    c[ImGuiCol_FrameBgActive]   = (ImVec4_c){ a, b, d, 0.38f };
    c[ImGuiCol_CheckMark]       = (ImVec4_c){ a, b, d, 1.0f };
    c[ImGuiCol_SliderGrab]      = (ImVec4_c){ a, b, d, 0.90f };
    c[ImGuiCol_SliderGrabActive]= (ImVec4_c){ a + 0.1f, b + 0.1f, d, 1.0f };
    c[ImGuiCol_Separator]       = (ImVec4_c){ a, b, d, 0.35f };
    c[ImGuiCol_Tab]             = (ImVec4_c){ a, b, d, 0.30f };
    c[ImGuiCol_TabHovered]      = (ImVec4_c){ a, b, d, 0.60f };
}

void menu_init(SDL_Window *win, SDL_GLContext gl)
{
    if (s_ctx) return;
    s_ctx = igCreateContext(NULL);
    igStyleColorsDark(NULL);
    menu_apply_style();
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

/* Case-insensitive substring test: does `name` contain `q`? Empty q = no match
 * (the caller only searches once the user has typed something). */
static int name_matches(const char *name, const char *q)
{
    size_t nl = strlen(name), ql = strlen(q);
    if (ql == 0 || ql > nl) return 0;
    for (size_t i = 0; i + ql <= nl; i++) {
        size_t k = 0;
        while (k < ql &&
               tolower((unsigned char)name[i + k]) == tolower((unsigned char)q[k]))
            k++;
        if (k == ql) return 1;
    }
    return 0;
}

/* Jump the camera to body `idx`, framing it from a slightly raised angle.
 * Camera position is in AU (g_cam.pos); body positions/radii are in metres, so
 * scale by RS (= 1/AU).  The view distance scales with the body's radius so a
 * star and a moon are each well-framed, with a small floor for radius-less
 * catalogue entries.  yaw/pitch are set to look straight at the target using the
 * same convention as cam_get_dir()/the inspect camera (yaw=atan2(z,x),
 * pitch=asin(y) on the camera→target unit vector). */
static void teleport_to_body(int idx)
{
    if (idx < 0 || idx >= g_nbodies) return;
    const Body *b = &g_bodies[idx];

    double tx = b->pos[0] * RS, ty = b->pos[1] * RS, tz = b->pos[2] * RS;  /* AU */
    double radius_au = b->radius * RS;
    double view_dist = fmax(radius_au * 6.0, radius_au + 1.0e-4);

    /* Approach direction (camera → target): from above and to one side. */
    double dx = 1.0, dy = -0.32, dz = 1.0;
    double dl = sqrt(dx*dx + dy*dy + dz*dz);
    dx /= dl; dy /= dl; dz /= dl;

    g_cam.pos[0] = tx - dx * view_dist;
    g_cam.pos[1] = ty - dy * view_dist;
    g_cam.pos[2] = tz - dz * view_dist;
    g_cam.yaw   = (float)(atan2(dz, dx) * 180.0 / PI);
    g_cam.pitch = (float)(asin(dy) * 180.0 / PI);
}

/* Render the "Navigate" tab: a name search over every live body with a
 * click-to-teleport result list. */
static void menu_render_navigate(void)
{
    static char s_query[64] = "";

    igTextDisabled("Search for a star or object, then click to jump to it.");
    igSpacing();
    igPushItemWidth(-1.0f);
    igInputTextWithHint("##search", "type a name (e.g. Proxima, Jupiter, Kepler)",
                        s_query, sizeof(s_query), 0, NULL, NULL);
    igPopItemWidth();
    igSpacing();

    if (!s_query[0]) {
        igTextDisabled("Start typing to see matches.");
        return;
    }

    igBeginChild_Str("##results", (ImVec2_c){ 0.0f, 0.0f },
                     ImGuiChildFlags_Borders, 0);
    const int MAX_RESULTS = 300;
    int shown = 0, capped = 0;
    for (int i = 0; i < g_nbodies; i++) {
        if (!g_bodies[i].alive) continue;
        if (!name_matches(g_bodies[i].name, s_query)) continue;
        if (shown >= MAX_RESULTS) { capped = 1; break; }

        /* "##tp%d" keeps each Selectable's ID unique (names can repeat, e.g.
         * many planets named "b"); only the text before "##" is shown. */
        char label[80];
        snprintf(label, sizeof(label), "%s %s##tp%d",
                 g_bodies[i].is_star ? "*" : " ", g_bodies[i].name, i);
        if (igSelectable_Bool(label, false, 0, (ImVec2_c){ 0.0f, 0.0f }))
            teleport_to_body(i);
        shown++;
    }
    if (shown == 0)
        igTextDisabled("No matches.");
    else if (capped)
        igTextDisabled("... more matches; refine the search.");
    igEndChild();
}

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
        igSetNextWindowSize((ImVec2_c){ 480.0f, 580.0f }, ImGuiCond_FirstUseEver);

        bool open = true;   /* window close [x] -> open=false */
        if (igBegin("Multiverse", &open, ImGuiWindowFlags_NoCollapse)) {

          if (igBeginTabBar("##mainTabs", ImGuiTabBarFlags_None)) {

           /* ===== Universe tab: presets, laws, import, save/load ========= */
           if (igBeginTabItem("Universe", NULL, 0)) {
            igSpacing();

            /* ---- Universes ---------------------------------------------- */
            if (igCollapsingHeader_TreeNodeFlags("Universes",
                                                 ImGuiTreeNodeFlags_DefaultOpen)) {
                igBeginChild_Str("##presets", (ImVec2_c){ 0.0f, 170.0f },
                                 ImGuiChildFlags_Borders, 0);
                for (int i = 0; i < preset_count(); i++) {
                    const UniversePreset *p = preset_at(i);
                    if (!p) continue;
                    if (igSelectable_Bool(p->name, i == current_preset,
                                          ImGuiSelectableFlags_None,
                                          (ImVec2_c){ 0.0f, 0.0f }))
                        switch_to = i;
                    if (p->blurb && p->blurb[0])
                        igSetItemTooltip("%s", p->blurb);
                }
                igEndChild();

                const UniversePreset *cur = preset_at(current_preset);
                if (cur && cur->blurb && cur->blurb[0]) {
                    igSpacing();
                    igPushTextWrapPos(0.0f);
                    igTextDisabled("%s", cur->blurb);
                    igPopTextWrapPos();
                }
            }

            igSpacing();
            /* ---- Physics laws ------------------------------------------- */
            if (igCollapsingHeader_TreeNodeFlags("Physics laws",
                                                 ImGuiTreeNodeFlags_DefaultOpen)) {
                /* Edit copies in human-friendly units, then write back. */
                float g_ratio = (float)(g_laws.G / LAWS_DEFAULT_G);
                float fexp    = (float)g_laws.force_exp;
                float tscale  = (float)g_laws.time_scale;
                float lam_e15 = (float)(g_laws.lambda * 1.0e15);
                float pn      = (float)g_laws.pn_factor;

                igPushItemWidth(igGetContentRegionAvail().x * 0.55f);
                int ch = 0;
                ch |= igSliderFloat("Gravity", &g_ratio, 0.1f, 10.0f, "%.2f x",
                                    ImGuiSliderFlags_Logarithmic);
                igSetItemTooltip("Strength of gravity vs. our universe (1.0 = real).");
                ch |= igSliderFloat("Force falloff", &fexp, 1.0f, 4.0f, "1/r^%.2f", 0);
                igSetItemTooltip("How gravity weakens with distance.\n"
                                 "2 = normal inverse-square; other values give "
                                 "exotic, precessing orbits.");
                ch |= igSliderFloat("Time scale", &tscale, 0.1f, 20.0f, "%.2f x",
                                    ImGuiSliderFlags_Logarithmic);
                igSetItemTooltip("How fast this universe's clock runs.");
                ch |= igSliderFloat("Dark energy", &lam_e15, -20.0f, 20.0f, "%.1f", 0);
                igSetItemTooltip("Cosmological push/pull (x1e-15).\n"
                                 "Positive expands wide orbits, negative contracts.");
                ch |= igSliderFloat("Relativity", &pn, 0.0f, 5.0e6f, "%.0f", 0);
                igSetItemTooltip("Post-Newtonian precession strength (0 = off).");
                igPopItemWidth();

                bool isol = (g_laws.gravity_isolation != 0.0);
                if (igCheckbox("Isolate star systems", &isol)) {
                    g_laws.gravity_isolation = isol ? 1.0 : 0.0;
                    if (laws_changed) *laws_changed = 1;
                }
                igSetItemTooltip("Ignore the (negligible) gravity between separate "
                                 "star systems.\nOn = far faster for big universes; "
                                 "off = every star pulls every other.");

                if (ch) {
                    g_laws.G          = (double)g_ratio * LAWS_DEFAULT_G;
                    g_laws.force_exp  = (double)fexp;
                    g_laws.time_scale = (double)tscale;
                    g_laws.lambda     = (double)lam_e15 * 1.0e-15;
                    g_laws.pn_factor  = (double)pn;
                    if (laws_changed) *laws_changed = 1;
                }

                igSpacing();
                if (igButton("Reset to Newtonian", (ImVec2_c){ -1.0f, 0.0f })) {
                    g_laws.G          = LAWS_DEFAULT_G;
                    g_laws.force_exp  = LAWS_DEFAULT_FORCE_EXP;
                    g_laws.time_scale = 1.0;
                    g_laws.lambda     = 0.0;
                    g_laws.pn_factor  = 0.0;
                    g_laws.gravity_isolation = LAWS_DEFAULT_GRAV_ISOLATION;
                    if (laws_changed) *laws_changed = 1;
                }
            }

            igSpacing();
            /* ---- Import real data --------------------------------------- */
            if (igCollapsingHeader_TreeNodeFlags("Import real data", 0)) {
                igTextDisabled("Build a universe from a real astronomical catalog.");
                igSpacing();
                for (int i = 0; i < (int)(sizeof(s_catalogs)/sizeof(s_catalogs[0])); i++) {
                    if (igButton(s_catalogs[i].label, (ImVec2_c){ -1.0f, 0.0f })) {
                        int n = catalog_convert((CatalogType)s_catalogs[i].type,
                                                s_catalogs[i].path, IMPORT_CACHE_PATH, 0);
                        if (n >= 0 && out_load_path) *out_load_path = IMPORT_CACHE_PATH;
                    }
                    igSetItemTooltip("%s", s_catalogs[i].path);
                }
            }

            igSpacing();
            /* ---- Save / load snapshot ----------------------------------- */
            if (igCollapsingHeader_TreeNodeFlags("Save / load snapshot", 0)) {
                static char s_file[256] = "assets/universes/mysave.json";
                igPushItemWidth(-1.0f);
                igInputText("##file", s_file, sizeof(s_file), 0, NULL, NULL);
                igPopItemWidth();
                igSpacing();
                float bw = (igGetContentRegionAvail().x
                            - igGetStyle()->ItemSpacing.x) * 0.5f;
                if (igButton("Save", (ImVec2_c){ bw, 0.0f }))
                    universe_save(s_file);
                igSetItemTooltip("Write the current live state to the file above.");
                igSameLine(0.0f, -1.0f);
                if (igButton("Load", (ImVec2_c){ bw, 0.0f }) && out_load_path)
                    *out_load_path = s_file;
                igSetItemTooltip("Load the universe from the file above.");
            }

            igEndTabItem();   /* Universe */
           }

           /* ===== Navigate tab: search for an object and teleport ======== */
           if (igBeginTabItem("Navigate", NULL, 0)) {
            igSpacing();
            menu_render_navigate();
            igEndTabItem();
           }

           igEndTabBar();
          }

            igSpacing();
            igSeparator();
            igTextDisabled("Press  U  to close   -   drag the title bar to move");
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
