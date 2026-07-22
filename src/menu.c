/*
 * menu.c — Dear ImGui (cimgui) universe picker + live-laws overlay.
 *
 * Active only when built with USE_IMGUI (make IMGUI=1). Otherwise the functions
 * below are no-op stubs so the rest of the program is unaffected.
 */
#include "menu.h"
#include "presets.h"
#include "laws.h"
#include "settings.h"
#include "catalog.h"
#include "universe.h"
#include "camera.h"
#include "body.h"
#include "post.h"
#include "nebula.h"
#include "galaxy.h"
#include "starsys.h"
#include "inspect.h"
#include "lifecycle.h"
#include "field_graph.h"
#include "spectral.h"
#include "physics.h"
#include <math.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

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
    { "Black holes — real",            "assets/catalogs/black_holes.csv",       CATALOG_BLACK_HOLES},
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

    double target[3] = {
        tx - dx * view_dist,
        ty - dy * view_dist,
        tz - dz * view_dist,
    };
    float yaw   = (float)(atan2(dz, dx) * 180.0 / PI);
    float pitch = (float)(asin(dy) * 180.0 / PI);

    /* Fly there with an eased animation rather than snapping; on arrival,
     * inspect mode auto-targets the body (orbit camera picks it up). */
    cam_fly_to(target, yaw, pitch);
    cam_fly_set_arrival_body(idx);
}

/* Fly to nebula `i`, framing it from outside at a few radii out and looking at
 * its centre. Nebulae are real world objects now, so this is a real teleport
 * (same eased flight as teleport_to_body); from the framed view you can fly the
 * rest of the way in to be enveloped by the volume. */
static void teleport_to_nebula(int i)
{
    double pos[3];
    nebula_position(i, pos);
    double radius    = nebula_radius_au(i);
    double view_dist = fmax(radius * 2.5, 1.0);

    double dx = 1.0, dy = -0.32, dz = 1.0;
    double dl = sqrt(dx*dx + dy*dy + dz*dz);
    dx /= dl; dy /= dl; dz /= dl;

    double target[3] = {
        pos[0] - dx * view_dist,
        pos[1] - dy * view_dist,
        pos[2] - dz * view_dist,
    };
    float yaw   = (float)(atan2(dz, dx) * 180.0 / PI);
    float pitch = (float)(asin(dy) * 180.0 / PI);
    cam_fly_to(target, yaw, pitch);
}

/* Fly to galaxy `i` — same framing as teleport_to_nebula. */
static void teleport_to_galaxy(int i)
{
    double pos[3];
    galaxy_position(i, pos);
    double radius    = galaxy_radius_au(i);
    double view_dist = fmax(radius * 2.5, 1.0);

    double dx = 1.0, dy = -0.32, dz = 1.0;
    double dl = sqrt(dx*dx + dy*dy + dz*dz);
    dx /= dl; dy /= dl; dz /= dl;

    double target[3] = {
        pos[0] - dx * view_dist,
        pos[1] - dy * view_dist,
        pos[2] - dz * view_dist,
    };
    float yaw   = (float)(atan2(dz, dx) * 180.0 / PI);
    float pitch = (float)(asin(dy) * 180.0 / PI);
    cam_fly_to(target, yaw, pitch);
}

/* ── Navigate: human-friendly mass/radius formatting ───────────────────────
 * Bodies range from moons to supermassive holes, so pick the unit that keeps
 * the number readable rather than forcing a single scale (cf. the Inspect tab,
 * which always uses Msun/Rsun). */
#define M_JUP_KG    1.898e27
#define M_EARTH_KG  5.972e24
#define R_SUN_M     6.9634e8
#define R_JUP_M     7.1492e7
#define R_EARTH_M   6.371e6

static void nav_fmt_mass(double kg, char *out, size_t n)
{
    if (kg <= 0.0)                       snprintf(out, n, "—");
    else if (kg >= 0.05 * SOLAR_MASS_KG) snprintf(out, n, "%.3g Msun",   kg / SOLAR_MASS_KG);
    else if (kg >= 0.05 * M_JUP_KG)      snprintf(out, n, "%.3g Mjup",   kg / M_JUP_KG);
    else                                 snprintf(out, n, "%.3g Mearth", kg / M_EARTH_KG);
}

static void nav_fmt_radius(double m, char *out, size_t n)
{
    if (m <= 0.0)                  snprintf(out, n, "—");
    else if (m >= 0.1 * R_SUN_M)   snprintf(out, n, "%.3g Rsun",   m / R_SUN_M);
    else if (m >= 0.1 * R_JUP_M)   snprintf(out, n, "%.3g Rjup",   m / R_JUP_M);
    else if (m >= 0.1 * R_EARTH_M) snprintf(out, n, "%.3g Rearth", m / R_EARTH_M);
    else                           snprintf(out, n, "%.3g km",     m / 1000.0);
}

/* Sort key/direction for the Navigate result list. File-static so nav_cmp() can
 * read them (qsort's comparator carries no context in C99). */
enum { NAV_SORT_NAME = 0, NAV_SORT_MASS, NAV_SORT_RADIUS };
static int s_nav_sort  = NAV_SORT_NAME;
static int s_nav_desc  = 0;

/* Total order over doubles that stays a strict weak ordering even if a value is
 * NaN (NaN sorts after all finite values; two NaNs compare equal). A naive
 * (x>y)-(x<y) returns 0 for every NaN comparison — non-transitive — which is
 * undefined behaviour for qsort and can read out of bounds in glibc. */
static int nav_cmp_double(double x, double y)
{
    int xn = isnan(x), yn = isnan(y);
    if (xn || yn) return xn - yn;
    return (x > y) - (x < y);
}

static int nav_cmp(const void *pa, const void *pb)
{
    int ia = *(const int *)pa, ib = *(const int *)pb;
    const Body *a = &g_bodies[ia], *b = &g_bodies[ib];
    int r;
    switch (s_nav_sort) {
        case NAV_SORT_MASS:
            r = nav_cmp_double(a->mass, b->mass);     break;
        case NAV_SORT_RADIUS:
            r = nav_cmp_double(a->radius, b->radius); break;
        default:
            r = strcasecmp(a->name, b->name);         break;
    }
    if (r == 0) r = ia - ib;              /* stable tiebreak by slot index */
    return s_nav_desc ? -r : r;
}

/* Render the "Navigate" tab: a filterable, sortable list over every live body
 * with a click-to-teleport result table, plus catalogue nebula/galaxy lists
 * that fly the camera to the chosen object. */
static void menu_render_navigate(void)
{
    static char s_query[64] = "";

    igTextDisabled("Search for a star or object to jump to, "
                   "or pick a nebula to fly to.");
    igSpacing();

    /* Nebulae are real world objects: list them all (only a handful) and fly
     * to the one clicked, framed from outside. */
    if (igCollapsingHeader_TreeNodeFlags("Nebulae (fly to)", 0)) {
        for (int i = 0; i < nebula_count(); i++) {
            char label[96];
            snprintf(label, sizeof(label), "~ %s##neb%d", nebula_name(i), i);
            if (igSelectable_Bool(label, false, 0, (ImVec2_c){ 0.0f, 0.0f }))
                teleport_to_nebula(i);
        }
    }
    /* Galaxies too (Layer 4.2) — same fly-to framing. */
    if (igCollapsingHeader_TreeNodeFlags("Galaxies (fly to)", 0)) {
        for (int i = 0; i < galaxy_count(); i++) {
            char label[96];
            snprintf(label, sizeof(label), "@ %s##gal%d", galaxy_name(i), i);
            if (igSelectable_Bool(label, false, 0, (ImVec2_c){ 0.0f, 0.0f }))
                teleport_to_galaxy(i);
        }
    }
    igSpacing();
    igPushItemWidth(-1.0f);
    igInputTextWithHint("##search", "filter by name (blank = list everything)",
                        s_query, sizeof(s_query), 0, NULL, NULL);
    igPopItemWidth();

    /* Type filter — combineable checkboxes. */
    static bool s_show_stars   = true;
    static bool s_show_planets = true;
    static bool s_show_holes   = true;
    igCheckbox("Stars", &s_show_stars);            igSameLine(0.0f, 16.0f);
    igCheckbox("Planets/moons", &s_show_planets);  igSameLine(0.0f, 16.0f);
    igCheckbox("Black holes", &s_show_holes);

    /* Sort key + direction — combines with the filter above. */
    igPushItemWidth(180.0f);
    igCombo_Str("Sort by", &s_nav_sort, "Name\0Mass\0Radius\0", -1);
    igPopItemWidth();
    igSameLine(0.0f, 16.0f);
    bool desc = s_nav_desc != 0;
    if (igCheckbox("Descending", &desc)) s_nav_desc = desc;
    igSpacing();

    /* Collect the alive bodies passing the type filter + name query, then sort
     * the index list by the chosen key (buffer grows as the universe does).
     *
     * The scan+qsort is cached and only rebuilt when an input that affects the
     * result changes (body count, query, filters, sort key/direction): with a
     * blank filter over a 100k+ star import this otherwise ran an O(n) scan plus
     * an O(n log n) sort in the UI thread every single frame. A body dying with
     * g_nbodies unchanged can leave a stale row until the next rebuild, which is
     * harmless — teleport_to_body re-validates the target's alive flag. */
    static int *s_idx = NULL;
    static int  s_idx_cap = 0;
    static int  s_nmatch  = 0;
    static int  s_c_nbodies = -1, s_c_sort = -1, s_c_desc = -1;
    static unsigned s_c_gen = (unsigned)-1;
    static char s_c_query[64] = "\x01";   /* impossible initial value → first build */
    static bool s_c_stars = false, s_c_planets = false, s_c_holes = false;

    if (s_idx_cap < g_nbodies) {
        int cap = g_nbodies > 0 ? g_nbodies : 1;
        int *p = (int *)realloc(s_idx, (size_t)cap * sizeof(int));
        if (p) { s_idx = p; s_idx_cap = cap; }
    }

    int rebuild = s_c_nbodies != g_nbodies || s_c_sort != s_nav_sort ||
                  s_c_desc != s_nav_desc || s_c_stars != s_show_stars ||
                  s_c_planets != s_show_planets || s_c_holes != s_show_holes ||
                  s_c_gen != g_universe_generation ||
                  strcmp(s_c_query, s_query) != 0;
    if (rebuild) {
        s_c_nbodies = g_nbodies; s_c_sort = s_nav_sort; s_c_desc = s_nav_desc;
        s_c_gen = g_universe_generation;
        s_c_stars = s_show_stars; s_c_planets = s_show_planets; s_c_holes = s_show_holes;
        snprintf(s_c_query, sizeof(s_c_query), "%s", s_query);

        int nmatch = 0;
        for (int i = 0; i < g_nbodies && nmatch < s_idx_cap; i++) {
            const Body *b = &g_bodies[i];
            if (!b->alive) continue;
            int is_hole   = b->is_black_hole;
            int is_star   = b->is_star && !is_hole;
            int is_planet = !b->is_star;
            if (is_hole   && !s_show_holes)   continue;
            if (is_star   && !s_show_stars)   continue;
            if (is_planet && !s_show_planets) continue;
            if (s_query[0] && !name_matches(b->name, s_query)) continue;
            s_idx[nmatch++] = i;
        }
        if (nmatch > 1)
            qsort(s_idx, (size_t)nmatch, sizeof(int), nav_cmp);
        s_nmatch = nmatch;
    }
    int nmatch = s_nmatch;

    const int MAX_RESULTS = 512;
    int shown = nmatch < MAX_RESULTS ? nmatch : MAX_RESULTS;
    int capped = nmatch > MAX_RESULTS;

    if (nmatch == 0) {
        igTextDisabled("No matches.");
        return;
    }

    ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_BordersInnerV;
    if (igBeginTable("##navresults", 3, tflags, (ImVec2_c){ 0.0f, 0.0f }, 0.0f)) {
        igTableSetupColumn("Name",   ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
        igTableSetupColumn("Mass",   ImGuiTableColumnFlags_WidthFixed,  110.0f, 0);
        igTableSetupColumn("Radius", ImGuiTableColumnFlags_WidthFixed,  110.0f, 0);
        igTableHeadersRow();

        /* Clip to the visible scroll region: only the ~dozen on-screen rows get
         * formatted + emitted each frame, instead of all `shown` (up to 512).
         * Rows are uniform height, so the clipper can seek without measuring. */
        ImGuiListClipper *clip = ImGuiListClipper_ImGuiListClipper();
        ImGuiListClipper_Begin(clip, shown, -1.0f);
        while (ImGuiListClipper_Step(clip)) {
            for (int r = clip->DisplayStart; r < clip->DisplayEnd; r++) {
                int i = s_idx[r];
                const Body *b = &g_bodies[i];
                char mass_s[32], rad_s[32], label[80];
                nav_fmt_mass(b->mass, mass_s, sizeof(mass_s));
                nav_fmt_radius(b->radius, rad_s, sizeof(rad_s));

                /* "##tp%d" keeps each Selectable's ID unique (names can repeat,
                 * e.g. many planets named "b"); the glyph flags the body kind. */
                const char *glyph = b->is_black_hole ? "o" : b->is_star ? "*" : " ";
                snprintf(label, sizeof(label), "%s %s##tp%d", glyph, b->name, i);

                igTableNextRow(0, 0.0f);
                igTableSetColumnIndex(0);
                if (igSelectable_Bool(label, false, ImGuiSelectableFlags_SpanAllColumns,
                                      (ImVec2_c){ 0.0f, 0.0f }))
                    teleport_to_body(i);
                igTableSetColumnIndex(1);
                igText("%s", mass_s);
                igTableSetColumnIndex(2);
                igText("%s", rad_s);
            }
        }
        ImGuiListClipper_End(clip);
        ImGuiListClipper_destroy(clip);
        igEndTable();
    }

    if (capped)
        igTextDisabled("Showing first %d of %d — refine the name filter.",
                       MAX_RESULTS, nmatch);
    else
        igTextDisabled("%d object%s.", nmatch, nmatch == 1 ? "" : "s");
}

/* Render the "Inspect" tab: details of the currently inspected body, plus
 * stellar-lifecycle controls when it is a star. */
static void menu_render_inspect(void)
{
    int t = g_inspect_target;
    if (t < 0 || t >= g_nbodies || !g_bodies[t].alive) {
        igTextDisabled("No object inspected.\n"
                       "Aim at a body and enter inspect mode to select it.");
        return;
    }
    Body *b = &g_bodies[t];
    igText("%s", b->name);
    igTextDisabled("%s", b->is_black_hole ? "Black hole" :
                         b->is_star ? "Star" : "Planet / moon");
    igSpacing();
    igText("Mass    %.3g Msun", b->mass / SOLAR_MASS_KG);
    igText("Radius  %.3g Rsun", b->radius / 6.9634e8);
    igSpacing();

    if (b->is_black_hole) {
        igSeparator();
        igText("Accretion (AGN engine)");
        igSpacing();
        igText("Spin a*          %.4f", b->spin_a);
        igText("Eddington ratio  %.3g  (L/L_edd)", b->eddington_ratio);
        igText("Accretion rate   %.3g Msun/yr",
               b->mdot * 3.15576e7 / SOLAR_MASS_KG);
        igText("Gas reservoir    %.3g Msun", b->gas_reservoir / SOLAR_MASS_KG);
        igTextDisabled(b->gas_reservoir > 0.0
                       ? "Fuelled — activity drives the disk/jets/torus."
                       : "Starved — quiescent hole.");
        igTextDisabled("Advance Stellar time to evolve (fade + grow).");
        igSpacing();
    }

    if (b->is_star && !b->is_black_hole) {
        igSeparator();
        igText("Stellar lifecycle");
        igSpacing();
        igText("Phase: %s", lifecycle_phase_name(b->star_phase));
        {
            SpectralClass sc;
            if (spectral_classify(b, &sc))
                igText("Class: %s  (T_eff ~ %.0f K)", sc.class_str, sc.t_eff);
        }

        if (lifecycle_is_evolvable_star(t)) {
            double tau = b->ms_lifetime_yr > 0.0 ? b->ms_lifetime_yr
                                                 : lifecycle_ms_lifetime_yr(b->mass);
            int death_remnant = 0;   /* >0 = a death just spawned this remnant */
            igTextDisabled("Main-sequence lifetime ~ %.3g yr", tau);
            if (b->age_yr > 0.0)
                igTextDisabled("Age ~ %.3g yr (%.0f%% of lifetime)",
                               b->age_yr, 100.0 * b->age_yr / tau);
            igSpacing();

            if (igButton("Age to next phase", (ImVec2_c){ -1.0f, 0.0f }))
                death_remnant = lifecycle_advance_phase(t);
            igSetItemTooltip("Step main sequence -> subgiant -> red giant -> death.");

            igSpacing();
            int sn = lifecycle_will_supernova(b->mass);
            if (igButton(sn ? "Trigger Supernova"
                            : "Collapse (planetary nebula)",
                         (ImVec2_c){ -1.0f, 0.0f }))
                death_remnant = lifecycle_trigger_death(t);
            igSetItemTooltip(sn
                ? "Core-collapse now: blow off the envelope and leave a neutron\n"
                  "star or black hole, kicking nearby bodies."
                : "End this low-mass star now: a gentle nebula puff leaving a\n"
                  "white dwarf.");

            /* A death just happened. Inspect mode pauses the sim, which would
             * freeze the explosion mid-flash forever, so resume so it plays out
             * and clears — and refocus the orbit camera on the new remnant
             * (the progenitor it was orbiting no longer exists). */
            if (death_remnant > 0) {
                g_paused = 0;
                inspect_focus_body(death_remnant);
            }
        } else {
            igTextDisabled("This object has reached its final state.");
        }
    }

    /* ── Relations: this body's field-graph edges (roadmap §0.4) ────────── */
    igSpacing();
    igSeparator();
    igText("Relations");
    igSpacing();

    /* Orbit chain (gravity edges): walk the parent hierarchy. */
    {
        char   chain[128] = "";
        size_t len = 0;
        int    depth = 0;
        for (int p = b->parent;
             p >= 0 && p < g_nbodies && g_bodies[p].alive &&
             depth < 8 && len < sizeof(chain);
             p = g_bodies[p].parent, depth++)
            len += (size_t)snprintf(chain + len, sizeof(chain) - len, "%s%s",
                                    depth ? " < " : "", g_bodies[p].name);
        if (depth > 0) igText("Orbits    %s", chain);
        else           igTextDisabled("Orbits    nothing (system root)");

        int children = 0;
        for (int i = 0; i < g_nbodies; i++) {
            /* Field stars are system roots (parent == -1), never children of an
             * inspected body — skip the ~263k-body bulk range instead of
             * scanning it every frame the Inspect tab is open. */
            if (i >= g_field_star_begin && i < g_field_star_end) {
                i = g_field_star_end - 1;
                continue;
            }
            if (g_bodies[i].alive && g_bodies[i].parent == t) children++;
        }
        if (children > 0)
            igText("Children  %d bodies in orbit", children);
    }

    /* Gas-flow edges: Roche mass-transfer streams and tidal disruption. */
    {
        FieldGraphEdge ed[8];
        int n = field_graph_body_edges(t, ed, 8);
        for (int i = 0; i < n; i++) {
            if (ed[i].type != FG_EDGE_GAS_FLOW) continue;
            const char *other = g_bodies[ed[i].from == t ? ed[i].to
                                                         : ed[i].from].name;
            const char *verb  = ed[i].from == t ? "Feeds " : "Fed by";
            if (ed[i].flow_kind == FG_FLOW_ROCHE)
                igText("%s    %s  (%.3g Msun/yr Roche stream)", verb, other,
                       ed[i].rate_kg_s * 3.15576e7 / SOLAR_MASS_KG);
            else
                igText("%s    %s  (tidal stream)", verb, other);
        }
    }

    /* Incident light: lazy radiation edges from the RadianceField. */
    {
        RadianceContrib rc[3];
        int n = field_graph_radiation_top(t, 3, rc);
        for (int i = 0; i < n; i++)
            igText("Light     %s  (%.3g W/m2)",
                   rc[i].body >= 0 ? g_bodies[rc[i].body].name : "supernova",
                   rc[i].irr);
        if (n == 0)
            igTextDisabled("Light     no emitters reach here");
    }

    /* This body's recorded history (evolution/event transitions). */
    {
        FieldGraphEvent ev[8];
        int n = field_graph_body_events(t, ev, 8);
        if (n > 0) {
            igSpacing();
            igText("History");
            for (int i = 0; i < n; i++) {
                if (ev[i].type == FG_EVENT_PHASE)
                    igText("  became %s", lifecycle_phase_name(ev[i].detail));
                else if (ev[i].type == FG_EVENT_SUPERNOVA)
                    igText("  supernova: %s -> %s (%s)", ev[i].a_name,
                           ev[i].b_name, lifecycle_phase_name(ev[i].detail));
                else
                    igText("  %s: %s absorbed %s",
                           field_graph_event_name(ev[i].type),
                           ev[i].a_name, ev[i].b_name);
            }
        } else {
            igTextDisabled("No recorded events.");
        }
    }

    igSpacing();
    igSeparator();
    igSpacing();
    /* Auto-aging rate: stellar evolution's own clock, independent of the
     * orbital speed control (which is capped for integrator stability). */
    float rate = (float)g_stellar_years_per_sec;
    igPushItemWidth(igGetContentRegionAvail().x * 0.55f);
    if (igSliderFloat("Stellar time", &rate, 0.0f, 2.0e6f, "%.0f yr/s", 0))
        g_stellar_years_per_sec = (double)rate;
    igPopItemWidth();
    igSetItemTooltip("How fast every star ages on its own clock.\n"
                     "0 = stars only change via the buttons above.\n"
                     "This does NOT speed up orbits.");

    /* Universe-wide event log (field graph), newest first. */
    igSpacing();
    if (igCollapsingHeader_TreeNodeFlags("Recent events", 0)) {
        FieldGraphEvent ev[10];
        int n = field_graph_events(ev, 10);
        if (n == 0)
            igTextDisabled("Nothing has happened yet.\n"
                           "Advance Stellar time, or make something collide.");
        for (int i = 0; i < n; i++) {
            if (ev[i].type == FG_EVENT_PHASE)
                igText("phase      %s -> %s", ev[i].a_name,
                       lifecycle_phase_name(ev[i].detail));
            else if (ev[i].type == FG_EVENT_SUPERNOVA)
                igText("supernova  %s -> %s (%s)", ev[i].a_name, ev[i].b_name,
                       lifecycle_phase_name(ev[i].detail));
            else
                igText("%-10s %s absorbed %s",
                       field_graph_event_name(ev[i].type),
                       ev[i].a_name, ev[i].b_name);
        }
    }
}

/* Global application settings (g_settings). Most changes take effect live;
 * num_stars and the overlay fonts own GL/TTF resources so they apply via a
 * button. Persisted to settings.json on exit (or "Save now" below). */
static void menu_render_settings(void)
{
    igTextDisabled("Global settings — apply to every universe.\n"
                   "Saved to settings.json on exit.");
    igSpacing();

    float w = 0.52f;

    if (igCollapsingHeader_TreeNodeFlags("Performance & scale",
                                         ImGuiTreeNodeFlags_DefaultOpen)) {
        float wr = (float)g_settings.warmup_radius_ly;
        float wy = (float)g_settings.warmup_years;
        float ar = (float)g_settings.active_radius_ly;
        igPushItemWidth(igGetContentRegionAvail().x * w);
        if (igSliderFloat("Warm-up radius", &wr, 0.0f, 10.0f, "%.2f ly", 0))
            g_settings.warmup_radius_ly = (double)wr;
        igSetItemTooltip("Systems this close to the start camera are pre-simulated\n"
                         "at load. Smaller = faster startup.");
        if (igSliderFloat("Warm-up time", &wy, 0.0f, 10.0f, "%.2f yr", 0))
            g_settings.warmup_years = (double)wy;
        igSetItemTooltip("Years of physics pre-simulated so orbits start spread out.");
        if (igSliderFloat("Active radius", &ar, 0.25f, 20.0f, "%.2f ly",
                          ImGuiSliderFlags_Logarithmic))
            g_settings.active_radius_ly = (double)ar;
        igSetItemTooltip("Radius of the live-simulated region each frame.\n"
                         "Larger = more bodies integrated = slower.");
        igPopItemWidth();
    }

    igSpacing();
    if (igCollapsingHeader_TreeNodeFlags("Starfield", 0)) {
        igPushItemWidth(igGetContentRegionAvail().x * w);
        igSliderInt("Fallback stars", &g_settings.num_stars, 0, 20000, "%d", 0);
        igSetItemTooltip("Procedural skybox star count (used when no BSC5 catalog).");
        igSliderInt("Background stars", &g_settings.bg_star_count, 0, 60000, "%d", 0);
        igSetItemTooltip("Faint star-dust layer between the catalog stars (0 = off).");
        igPopItemWidth();
        if (igButton("Regenerate starfield", (ImVec2_c){ -1.0f, 0.0f }))
            settings_apply_starfield();
    }

    igSpacing();
    if (igCollapsingHeader_TreeNodeFlags("Camera & controls", 0)) {
        igPushItemWidth(igGetContentRegionAvail().x * w);
        igSliderFloat("Field of view", &g_settings.fov, 30.0f, 110.0f, "%.0f deg", 0);
        igSliderFloat("Warp min", &g_settings.warp_speed_min_au, 1.0f, 5000.0f,
                      "%.0f AU/s", ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Warp max", &g_settings.warp_speed_max_au, 1000.0f, 200000.0f,
                      "%.0f AU/s", ImGuiSliderFlags_Logarithmic);
        {
            bool aw = g_settings.adaptive_warp != 0;
            if (igCheckbox("Adaptive warp (scale travel)", &aw))
                g_settings.adaptive_warp = aw ? 1 : 0;
            igSetItemTooltip("In warp, speed also scales with distance from the\n"
                             "nearest body: hold W to accelerate out of the system,\n"
                             "through interstellar space, and clear of the galaxy —\n"
                             "each 10x of scale takes a fixed few seconds.\n"
                             "Approaching anything decelerates automatically.");
        }
        igSliderFloat("Adjust step", &g_settings.slider_step, 0.01f, 0.5f, "%.2f", 0);
        igSetItemTooltip("Increment for keyboard/wheel volume & sensitivity changes.");
        igSliderFloat("Mouse sens min", &g_settings.mouse_sens_min, 0.01f, 1.0f, "%.2f", 0);
        igSliderFloat("Mouse sens max", &g_settings.mouse_sens_max, 0.1f, 5.0f, "%.2f", 0);
        igPopItemWidth();
    }

    igSpacing();
    if (igCollapsingHeader_TreeNodeFlags("Far-field fade (AU)", 0)) {
        igPushItemWidth(igGetContentRegionAvail().x * w);
        igSliderFloat("Trail fade start", &g_settings.sys_trail_fade_start, 0.0f, 5000.0f, "%.0f", 0);
        igSliderFloat("Trail fade end",   &g_settings.sys_trail_fade_end,   0.0f, 8000.0f, "%.0f", 0);
        igSliderFloat("Dot fade start",   &g_settings.sys_dot_fade_start,   0.0f, 5000.0f, "%.0f", 0);
        igSliderFloat("Dot fade end",     &g_settings.sys_dot_fade_end,     0.0f, 8000.0f, "%.0f", 0);
        igSliderFloat("Far-field horizon", &g_settings.farfield_horizon_au,
                      1.0e3f, 1.0e9f, "%.0f AU", ImGuiSliderFlags_Logarithmic);
        if (g_settings.farfield_horizon_au > RENDER_DEPTH_FAR)
            g_settings.farfield_horizon_au = RENDER_DEPTH_FAR;
        igPopItemWidth();
        igSetItemTooltip("Distance (AU) over which system trails / non-star dots fade out.\n"
                         "Far-field horizon: distant stars, glare and black holes render at\n"
                         "their true depth and fade/cull past this distance (1 ly = 63241 AU).");
    }

    igSpacing();
    if (igCollapsingHeader_TreeNodeFlags("Detail transitions (LOD)", 0)) {
        igPushItemWidth(igGetContentRegionAvail().x * w);
        igSliderFloat("Sphere fade start", &g_settings.lod_body_fade_start_px,
                      0.1f, 10.0f, "%.2f px", ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Sphere fade end",   &g_settings.lod_body_fade_end_px,
                      0.2f, 20.0f, "%.2f px", ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Glare fade start",  &g_settings.lod_glare_full_px,
                      0.1f, 10.0f, "%.2f px", ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Glare fade end",    &g_settings.lod_glare_fade_px,
                      0.2f, 40.0f, "%.2f px", ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Density LOD cap",   &g_settings.lod_density_max,
                      1.0f, 10.0f, "%.1fx", 0);
        igSliderFloat("Dot overlap hide",  &g_settings.dot_hide_px,
                      0.0f, 20.0f, "%.1f px", 0);
        igSliderFloat("Dot overlap free",  &g_settings.dot_excl_px,
                      0.5f, 40.0f, "%.1f px", 0);
        igSliderFloat("Near-dot range",    &g_settings.near_dot_dist_ly,
                      0.5f, 50.0f, "%.1f ly", ImGuiSliderFlags_Logarithmic);
        igSliderFloat("Cluster impostors", &g_settings.cluster_impostors,
                      0.0f, 3.0f, "%.2fx", 0);
        igCheckbox("Galaxy AGN nuclei", (bool *)&g_settings.galaxy_agn);
        igSetItemTooltip("Spawn a black-hole nucleus at each AGN host galaxy's centre\n"
                         "(M87 + Centaurus A active with jets; Sgr A* + Andromeda\n"
                         "quiescent). Fly to a galactic centre to see the accretion\n"
                         "disk + jets bloom inside the galaxy glow. Takes effect on the\n"
                         "next universe (re)load.");
        /* Keep each window well-ordered live (end > start) so the smoothstep
         * edges never coincide/cross while a slider is being dragged. */
        if (g_settings.lod_body_fade_end_px < g_settings.lod_body_fade_start_px + 0.05f)
            g_settings.lod_body_fade_end_px = g_settings.lod_body_fade_start_px + 0.05f;
        if (g_settings.lod_glare_fade_px < g_settings.lod_glare_full_px + 0.05f)
            g_settings.lod_glare_fade_px = g_settings.lod_glare_full_px + 0.05f;
        if (g_settings.dot_excl_px < g_settings.dot_hide_px + 0.1f)
            g_settings.dot_excl_px = g_settings.dot_hide_px + 0.1f;
        igPopItemWidth();
        igSetItemTooltip("Continuous-LOD crossfades: a body's dot fades out exactly as its\n"
                         "sphere (or a star's glare billboard) fades in over these projected-\n"
                         "pixel windows. 'Density LOD cap' bounds the CosmicField factor that\n"
                         "widens the windows in dense/clumped regions (1 = disable). Dot\n"
                         "overlap hide/free control screen-space dot dedup; near-dot range is\n"
                         "the radius of the full per-dot treatment (beyond it: cheap far dots).\n"
                         "'Cluster impostors' is the aggregate-glow intensity for dense star\n"
                         "clumps drawn as one blob when their members are sub-pixel/culled at\n"
                         "distance, fading out as the clump resolves on approach (0 = off).");
    }

    igSpacing();
    if (igCollapsingHeader_TreeNodeFlags("Labels", 0)) {
        igPushItemWidth(igGetContentRegionAvail().x * w);
        igSliderFloat("Max label distance", &g_settings.label_max_dist_au,
                      5.0f, 5000.0f, "%.0f AU", ImGuiSliderFlags_Logarithmic);
        igSliderInt("Pinned planets", &g_settings.label_pin_planets, 0, 16, "%d", 0);
        igSliderInt("Pinned systems", &g_settings.label_pin_systems, 0, 16, "%d", 0);
        igPopItemWidth();
        igSetItemTooltip("Planet/moon name labels are hidden beyond 'Max label distance'.\n"
                         "Stars are always labelled within the active region.\n"
                         "'Pinned planets/systems' keep the nearest N planets and M star\n"
                         "systems labelled at any range — pinned planets ignore the distance\n"
                         "cutoff and pinned systems show even outside the active region.");
    }

    igSpacing();
    if (igCollapsingHeader_TreeNodeFlags("Loading overlay", 0)) {
        igPushItemWidth(igGetContentRegionAvail().x * w);
        igSliderInt("Status font px", &g_settings.status_font_px, 8, 48, "%d", 0);
        igSliderInt("Percent font px", &g_settings.pct_font_px, 6, 36, "%d", 0);
        igPopItemWidth();
        if (igButton("Reload fonts", (ImVec2_c){ -1.0f, 0.0f }))
            settings_apply_fonts();
        igSeparator();

        float fin  = (float)g_settings.fade_in_dur;
        float fout = (float)g_settings.fade_out_dur;
        float pe   = (float)g_settings.prog_ease;
        float sw   = (float)g_settings.sweep_speed;
        float pdt  = (float)g_settings.present_dt;
        igPushItemWidth(igGetContentRegionAvail().x * w);
        if (igSliderFloat("Fade in",  &fin,  0.0f, 2.0f, "%.2f s", 0)) g_settings.fade_in_dur = (double)fin;
        if (igSliderFloat("Fade out", &fout, 0.0f, 2.0f, "%.2f s", 0)) g_settings.fade_out_dur = (double)fout;
        if (igSliderFloat("Progress ease", &pe, 1.0f, 30.0f, "%.1f", 0)) g_settings.prog_ease = (double)pe;
        if (igSliderFloat("Sweep speed", &sw, 0.1f, 3.0f, "%.2f /s", 0)) g_settings.sweep_speed = (double)sw;
        if (igSliderFloat("Present interval", &pdt, 0.002f, 0.05f, "%.3f s", 0)) g_settings.present_dt = (double)pdt;
        igPopItemWidth();

        float col[3] = { g_settings.accent_r, g_settings.accent_g, g_settings.accent_b };
        if (igColorEdit3("Accent", col, 0)) {
            g_settings.accent_r = col[0];
            g_settings.accent_g = col[1];
            g_settings.accent_b = col[2];
        }
    }

    igSpacing();
    if (igCollapsingHeader_TreeNodeFlags("Trails (advanced)", 0)) {
        float msl  = (float)g_settings.trail_min_segment_len;
        float xsl  = (float)g_settings.trail_max_segment_len;
        float bsl  = (float)g_settings.trail_base_segment_len;
        float ssl  = (float)g_settings.trail_satellite_segment_len;
        float caf  = (float)g_settings.trail_close_approach_factor;
        float cer  = (float)g_settings.trail_curve_error_ratio;
        igPushItemWidth(igGetContentRegionAvail().x * w);
        if (igSliderFloat("Min segment (m)", &msl, 1.0e3f, 1.0e6f, "%.0f",
                          ImGuiSliderFlags_Logarithmic)) g_settings.trail_min_segment_len = (double)msl;
        if (igSliderFloat("Max segment (m)", &xsl, 1.0e6f, 1.0e10f, "%.3g",
                          ImGuiSliderFlags_Logarithmic)) g_settings.trail_max_segment_len = (double)xsl;
        if (igSliderFloat("Base segment (m)", &bsl, 1.0e5f, 1.0e10f, "%.3g",
                          ImGuiSliderFlags_Logarithmic)) g_settings.trail_base_segment_len = (double)bsl;
        if (igSliderFloat("Satellite segment (m)", &ssl, 1.0e3f, 1.0e8f, "%.3g",
                          ImGuiSliderFlags_Logarithmic)) g_settings.trail_satellite_segment_len = (double)ssl;
        if (igSliderFloat("Close-approach factor", &caf, 0.05f, 1.0f, "%.2f", 0))
            g_settings.trail_close_approach_factor = (double)caf;
        if (igSliderFloat("Curve error ratio", &cer, 0.05f, 1.0f, "%.2f", 0))
            g_settings.trail_curve_error_ratio = (double)cer;
        igPopItemWidth();
        igTextDisabled("Retained-length and curve-error bounds use defaults; edit\n"
                       "settings.json directly for those.");
    }

    igSpacing();
    igSeparator();
    float bw = (igGetContentRegionAvail().x - igGetStyle()->ItemSpacing.x) * 0.5f;
    if (igButton("Reset to defaults", (ImVec2_c){ bw, 0.0f })) {
        settings_reset();
        settings_apply_fonts();
        settings_apply_starfield();
    }
    igSameLine(0.0f, -1.0f);
    if (igButton("Save now", (ImVec2_c){ bw, 0.0f }))
        settings_save();
}

int menu_render(int current_preset, int *laws_changed, const char **out_load_path)
{
    int switch_to = -1;
    if (laws_changed) *laws_changed = 0;
    if (!s_ctx) return -1;

    /* Menu closed (the common case during normal flight): skip the entire ImGui
     * frame. Running the SDL2/OpenGL3 backends' per-frame input+display setup and
     * submitting an empty draw list every frame is pure waste when nothing is
     * shown. The window is a hard toggle (no open/close animation), and
     * menu_process_event() already no-ops while closed, so nothing depends on
     * ImGui ticking here. */
    if (!s_visible) return -1;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    igNewFrame();

    {
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
                    universe_refresh_bh_radii();   /* horizons derive from G */
                    if (laws_changed) *laws_changed = 1;
                }

                igSpacing();
                if (igButton("Reset to Newtonian", (ImVec2_c){ -1.0f, 0.0f })) {
                    laws_reset();
                    universe_refresh_bh_radii();   /* horizons derive from G */
                    if (laws_changed) *laws_changed = 1;
                }

                /* ---- Timestep model (advanced, per-universe) ------------ */
                igSpacing();
                if (igTreeNode_Str("Timestep model (advanced)")) {
                    igTextDisabled("Integration accuracy vs. cost. Changes rebuild\n"
                                   "the per-system timestep model immediately.");
                    igSpacing();
                    float opd = (float)g_laws.outer_period_divisor;
                    float ipd = (float)g_laws.inner_period_divisor;
                    float odmin = (float)g_laws.outer_dt_min;   /* seconds */
                    float idmin = (float)g_laws.inner_dt_min;
                    float idmax = (float)g_laws.inner_dt_max;
                    float oddef = (float)g_laws.outer_dt_default;

                    igPushItemWidth(igGetContentRegionAvail().x * 0.50f);
                    int tch = 0;
                    tch |= igSliderFloat("Outer divisor", &opd, 4.0f, 192.0f, "T / %.0f", 0);
                    igSetItemTooltip("Slow (star/planet) step = orbital period / this.");
                    tch |= igSliderFloat("Inner divisor", &ipd, 8.0f, 512.0f, "T / %.0f", 0);
                    igSetItemTooltip("Fast (moon) substep = orbital period / this.");
                    tch |= igSliderFloat("Outer dt min", &odmin, 60.0f, 86400.0f, "%.0f s",
                                         ImGuiSliderFlags_Logarithmic);
                    igSetItemTooltip("Floor on the slow step (s). Stops one tight orbiter\n"
                                     "from throttling the whole scene.");
                    tch |= igSliderFloat("Inner dt min", &idmin, 1.0f, 600.0f, "%.0f s",
                                         ImGuiSliderFlags_Logarithmic);
                    igSetItemTooltip("Floor on the fast substep (s).\n"
                                     "WARNING: much below 60 s and physics diverges.");
                    tch |= igSliderFloat("Inner dt max", &idmax, 60.0f, 7200.0f, "%.0f s",
                                         ImGuiSliderFlags_Logarithmic);
                    igSetItemTooltip("Ceiling on the fast substep (s).");
                    tch |= igSliderFloat("Outer dt default", &oddef, 3600.0f, 864000.0f, "%.0f s",
                                         ImGuiSliderFlags_Logarithmic);
                    igSetItemTooltip("Ceiling / fallback for the slow step (s).");
                    igPopItemWidth();

                    if (tch) {
                        if (idmin < 1.0f) idmin = 1.0f;   /* hard floor: divergence */
                        g_laws.outer_period_divisor = (double)opd;
                        g_laws.inner_period_divisor = (double)ipd;
                        g_laws.outer_dt_min         = (double)odmin;
                        g_laws.inner_dt_min         = (double)idmin;
                        g_laws.inner_dt_max         = (double)idmax;
                        g_laws.outer_dt_default     = (double)oddef;
                        if (laws_changed) *laws_changed = 1;
                    }
                    igTreePop();
                }
            }

            igSpacing();
            /* ---- Visuals ------------------------------------------------ */
            if (igCollapsingHeader_TreeNodeFlags("Visuals", 0)) {
                if (!post_available()) {
                    igTextDisabled("Bloom unavailable (shader load failed).");
                } else {
                    int   en; float th, in;
                    post_get_bloom(&en, &th, &in);
                    bool  b = en;
                    int   ch = 0;
                    if (igCheckbox("Bloom", &b)) { en = b; ch = 1; }
                    igSetItemTooltip("HDR glow around bright stars and glare.");
                    igPushItemWidth(igGetContentRegionAvail().x * 0.55f);
                    ch |= igSliderFloat("Bloom threshold", &th, 0.0f, 2.0f, "%.2f", 0);
                    igSetItemTooltip("Brightness above which pixels start to glow.");
                    ch |= igSliderFloat("Bloom intensity", &in, 0.0f, 3.0f, "%.2f", 0);
                    igSetItemTooltip("How strongly the glow is added back.");
                    igPopItemWidth();
                    if (ch) post_set_bloom(en, th, in);

                    /* ---- Tonemapping (persisted in g_settings) -------- */
                    igSpacing();
                    int   tm = g_settings.tonemap_mode;
                    float ex = g_settings.tonemap_exposure;
                    int   tch = 0;
                    igText("Tonemap");
                    igSetItemTooltip("Filmic highlight roll-off. Off = original "
                                     "linear look. Needs Bloom (HDR target) on.");
                    tch |= igRadioButton_IntPtr("Off", &tm, 0);  igSameLine(0.0f, 12.0f);
                    tch |= igRadioButton_IntPtr("ACES", &tm, 1); igSameLine(0.0f, 12.0f);
                    tch |= igRadioButton_IntPtr("Reinhard", &tm, 2);
                    if (tm != 0) {
                        igPushItemWidth(igGetContentRegionAvail().x * 0.55f);
                        tch |= igSliderFloat("Exposure", &ex, 0.05f, 8.0f, "%.2f",
                                             ImGuiSliderFlags_Logarithmic);
                        igSetItemTooltip("Linear exposure before the tonemap curve.");
                        igPopItemWidth();
                    }
                    if (tch) {
                        g_settings.tonemap_mode     = tm;
                        g_settings.tonemap_exposure = ex;
                        post_set_tonemap(tm, ex);
                    }

                    /* ---- Lens optics (persisted in g_settings) -------- */
                    igSpacing();
                    int   ae = g_settings.auto_exposure;
                    float ca = g_settings.chromatic_aberration;
                    float vg = g_settings.vignette;
                    int   och = 0;
                    bool  aeb = ae;
                    if (igCheckbox("Auto exposure", &aeb)) { ae = aeb; och = 1; }
                    igSetItemTooltip("Adapt exposure to scene brightness "
                                     "(needs a tonemap mode on).");
                    igPushItemWidth(igGetContentRegionAvail().x * 0.55f);
                    och |= igSliderFloat("Chromatic aberration", &ca, 0.0f, 0.02f, "%.4f", 0);
                    igSetItemTooltip("Lateral colour fringing toward the edges.");
                    och |= igSliderFloat("Vignette", &vg, 0.0f, 1.0f, "%.2f", 0);
                    igSetItemTooltip("Darken the corners of the frame.");
                    igPopItemWidth();
                    if (och) {
                        g_settings.auto_exposure        = ae;
                        g_settings.chromatic_aberration = ca;
                        g_settings.vignette             = vg;
                        post_set_optics(ae, ca, vg);
                    }

                    float sp = g_settings.lens_spikes;
                    igPushItemWidth(igGetContentRegionAvail().x * 0.55f);
                    if (igSliderFloat("Star spikes", &sp, 0.0f, 1.5f, "%.2f", 0))
                        g_settings.lens_spikes = sp;
                    igSetItemTooltip("Diffraction spikes on bright stars.");
                    igSliderFloat("Lens flare", &g_settings.lens_flare, 0.0f, 1.0f, "%.2f", 0);
                    igSetItemTooltip("Ghost sprites + halo + anamorphic streak from the "
                                     "dominant sun (0 = off).");
                    igSliderFloat("Flare ghosts", &g_settings.flare_ghosts, 0.0f, 3.0f, "%.2f", 0);
                    igSetItemTooltip("Ghost-sprite chain mirrored through the screen "
                                     "centre (1 = calibrated look).");
                    igSliderFloat("Flare halo", &g_settings.flare_halo, 0.0f, 3.0f, "%.2f", 0);
                    igSetItemTooltip("Internal-reflection halo ring strength.");
                    igSliderFloat("Flare halo radius", &g_settings.flare_halo_radius, 0.1f, 0.7f, "%.2f", 0);
                    igSetItemTooltip("Halo ring radius in screen units.");
                    igSliderFloat("Flare streak", &g_settings.flare_streak, 0.0f, 3.0f, "%.2f", 0);
                    igSetItemTooltip("Blue anamorphic streak strength.");
                    igSliderFloat("Flare streak length", &g_settings.flare_streak_len, 0.2f, 3.0f, "%.2fx", 0);
                    igSetItemTooltip("Horizontal reach of the anamorphic streak.");
                    igSliderFloat("Flare core", &g_settings.flare_core, 0.0f, 3.0f, "%.2f", 0);
                    igSetItemTooltip("Warm glow anchoring the flare to the sun itself.");
                    igSliderFloat("Relativistic", &g_settings.relativistic, 0.0f, 1.0f, "%.2f", 0);
                    igSetItemTooltip("Aberration + Doppler shift at warp speed. "
                                     "Only visible while moving fast.");
                    igPopItemWidth();

                    /* ---- Stellar appearance (persisted in g_settings) - */
                    igSpacing();
                    igText("Stars");
                    igPushItemWidth(igGetContentRegionAvail().x * 0.55f);
                    igSliderFloat("Twinkle", &g_settings.star_twinkle, 0.0f, 1.0f, "%.2f", 0);
                    igSetItemTooltip("Subtle per-star brightness shimmer on the dot field.");
                    igSliderFloat("Corona", &g_settings.star_corona, 0.0f, 1.0f, "%.2f", 0);
                    igSetItemTooltip("Animated glare streamers, stronger on hot blue stars.");
                    igSliderFloat("Starspots", &g_settings.starspots, 0.0f, 1.0f, "%.2f", 0);
                    igSetItemTooltip("Granulation and dark spots on close-up star surfaces.");
                    igPopItemWidth();

                    /* ---- Auroras (persisted in g_settings) ------------ */
                    igSpacing();
                    igText("Auroras");
                    igPushItemWidth(igGetContentRegionAvail().x * 0.55f);
                    igSliderFloat("Aurora gain", &g_settings.aurora_gain, 0.0f, 10.0f, "%.1f", 0);
                    igSetItemTooltip("Curtain emission brightness at storm strength 1 "
                                     "(0 = auroras off).");
                    igSliderFloat("Oval latitude", &g_settings.aurora_oval_lat, 50.0f, 85.0f, "%.1f deg", 0);
                    igSetItemTooltip("Magnetic latitude of the quiet-time oval "
                                     "around each spin pole.");
                    igSliderFloat("Oval width", &g_settings.aurora_oval_width, 0.005f, 0.08f, "%.3f", 0);
                    igSetItemTooltip("Quiet-time oval thickness (gaussian half-width "
                                     "in sin-latitude).");
                    igSliderFloat("Storm expansion", &g_settings.aurora_storm_expand, 0.0f, 3.0f, "%.2f", 0);
                    igSetItemTooltip("How far storms push the oval equatorward and "
                                     "thicken it (0 = shape ignores storms).");
                    igSliderFloat("Red band", &g_settings.aurora_red, 0.0f, 1.0f, "%.2f", 0);
                    igSetItemTooltip("High-altitude red oxygen line. Keep low: oblique "
                                     "rays integrate the tall red column far longer "
                                     "than the thin green layer.");
                    igSliderFloat("Violet fringe", &g_settings.aurora_violet, 0.0f, 1.0f, "%.2f", 0);
                    igSetItemTooltip("Nitrogen fringe at the curtain's bottom edge.");
                    igSliderFloat("Storm floor", &g_settings.aurora_storm_base, 0.0f, 1.5f, "%.2f", 0);
                    igSetItemTooltip("Quiet-time activity level (the faint oval "
                                     "between storms).");
                    igSliderFloat("Storm amplitude", &g_settings.aurora_storm_amp, 0.0f, 5.0f, "%.1f", 0);
                    igSetItemTooltip("Peak storm strength on top of the floor "
                                     "(cubed noise, so big storms stay rare).");
                    igSliderFloat("Storm timescale", &g_settings.aurora_storm_scale, 0.1f, 10.0f, "%.2fx", 0);
                    igSetItemTooltip("Multiplies the substorm/storm/sector periods "
                                     "(~40 min / 5 h / 22 h sim time at 1x).");
                    igSliderFloat("Storm smoothing", &g_settings.aurora_smooth_s, 0.0f, 10.0f, "%.1f s", 0);
                    igSetItemTooltip("Real-time low-pass on activity so storms breathe "
                                     "instead of strobing at high sim rates.");
                    igPopItemWidth();
                }

                igSeparator();
                {
                    int   nen, nst; float nde;
                    nebula_get_params(&nen, &nde, &nst);
                    bool  nb = nen;
                    int   nch = 0;
                    if (igCheckbox("Nebulae", &nb)) { nen = nb; nch = 1; }
                    igSetItemTooltip("Real-catalogue volumetric nebulae you can fly into.");
                    igPushItemWidth(igGetContentRegionAvail().x * 0.55f);
                    nch |= igSliderFloat("Nebula density", &nde, 0.0f, 2.0f, "%.2f", 0);
                    igSetItemTooltip("Cloud opacity / brightness.");
                    nch |= igSliderInt("Nebula steps", &nst, 4, 48, "%d", 0);
                    igSetItemTooltip("Raymarch quality vs. performance. Distant nebulae "
                                     "use fewer steps automatically.");
                    igPopItemWidth();
                    if (nch) nebula_set_params(nen, nde, nst);
                }

                igSeparator();
                {
                    int   gen, gst, gsr; float gde;
                    galaxy_get_params(&gen, &gde, &gst, &gsr);
                    bool  gb = gen, sb = gsr;
                    int   gch = 0;
                    if (igCheckbox("Galaxies", &gb)) { gen = gb; gch = 1; }
                    igSetItemTooltip("Volumetric catalogue galaxies, including "
                                     "the Milky Way you're inside.");
                    igPushItemWidth(igGetContentRegionAvail().x * 0.55f);
                    gch |= igSliderFloat("Galaxy density", &gde, 0.0f, 2.0f, "%.2f", 0);
                    igSetItemTooltip("Volume glow opacity / brightness.");
                    gch |= igSliderInt("Galaxy steps", &gst, 4, 64, "%d", 0);
                    igSetItemTooltip("Raymarch quality vs. performance. Distant galaxies "
                                     "use fewer steps automatically.");
                    igPopItemWidth();
                    if (igCheckbox("Resolved stars", &sb)) { gsr = sb; gch = 1; }
                    igSetItemTooltip("Procedural point stars that resolve out of the "
                                     "galaxy glow as you approach.");
                    if (gch) galaxy_set_params(gen, gde, gst, gsr);

                    bool pb = starsys_enabled();
                    if (igCheckbox("Star promotion", &pb))
                        starsys_set_enabled(pb);
                    igSetItemTooltip("Approach a procedural star and it becomes a real "
                                     "planetary system. Off demotes any current ones.");
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

           /* ===== Inspect tab: body details + stellar lifecycle ========== */
           if (igBeginTabItem("Inspect", NULL, 0)) {
            igSpacing();
            menu_render_inspect();
            igEndTabItem();
           }

           /* ===== Settings tab: global app config (g_settings) =========== */
           if (igBeginTabItem("Settings", NULL, 0)) {
            igSpacing();
            menu_render_settings();
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
