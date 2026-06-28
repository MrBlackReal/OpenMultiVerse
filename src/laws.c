/*
 * laws.c — definition and defaults for the active universe law set.
 *
 * JSON parsing of the "laws" block lives in universe.c (which already owns the
 * JSON tree); this module only owns the global and its reset-to-Newtonian path.
 */
#include "laws.h"

UniverseLaws g_laws = {
    .G          = LAWS_DEFAULT_G,
    .softening  = LAWS_DEFAULT_SOFTENING,
    .time_scale = 1.0,
    .force_exp  = LAWS_DEFAULT_FORCE_EXP,
    .lambda     = 0.0,
    .pn_factor  = 0.0,
    .c_light    = LAWS_DEFAULT_C_LIGHT,
};

void laws_reset(void)
{
    g_laws.G          = LAWS_DEFAULT_G;
    g_laws.softening  = LAWS_DEFAULT_SOFTENING;
    g_laws.time_scale = 1.0;
    g_laws.force_exp  = LAWS_DEFAULT_FORCE_EXP;
    g_laws.lambda     = 0.0;
    g_laws.pn_factor  = 0.0;
    g_laws.c_light    = LAWS_DEFAULT_C_LIGHT;
}
