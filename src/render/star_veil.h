/*
 * star_veil.h — glare of a nearby star over the background (CINEMATIC.md §8.3a).
 *
 * A background source (star, cluster, galaxy or nebula pixel) stays visible
 * only while it outshines the star's glare AT ITS OWN POSITION on the sky:
 *
 *     glare(theta) = E_psf * VEIL_PSF / theta^2  +  E_floor * VEIL_FLOOR
 *     visible      = smoothstep(VEIL_LO, VEIL_HI, log2(lum / glare))
 *
 * Two scales, because they model two different things:
 *   E_psf   the star's glare point-spread, proportional to its LINEAR brightness
 *           at the camera (1/d^2). With a 1/theta^2 PSF the angle at which the
 *           glare reaches any given level then shrinks as 1/d, exactly like
 *           the disc itself, so the Sun's glow is huge from Mercury and a
 *           small point from Neptune. The visible haze (below) is this same
 *           term, so a patch drowned in glare always shows the glare.
 *   E_floor the overall exposure cut, on a compressed (square-root) scale so
 *           the washout eases off smoothly as you leave a system: a black sky
 *           at Earth, only bright stars at Neptune.
 * The 1/theta^2 shape means the sky drowns nearest the star first; the floor
 * means faint sources go everywhere before bright ones. Both come from
 * render.c. The same formula runs in C (CPU-built dots and glare billboards)
 * and in GLSL (every other background layer, via the gl_utils prelude), so the
 * two paths agree exactly.
 */
#pragma once
#include <math.h>

#define VEIL_PSF      100.0   /* PSF term is 1 at 10 degrees            */
#define VEIL_FLOOR    0.14    /* exposure floor, scaled by E_floor      */
#define VEIL_CORE_DEG 0.5     /* clamp inside the star's own disc       */
#define VEIL_LO      -1.0     /* log2 contrast: fully gone at 1/2 glare */
#define VEIL_HI       1.5     /* ...fully visible at 2.8x the glare     */
/* Star sprites are drawn HDR-overbright for visibility (~0.2-3) while a Milky
 * Way or nebula pixel sits near 0.03, so a like-for-like test drowned the
 * diffuse glow long before stars of similar visual weight. Diffuse layers
 * enter the test (not their drawn brightness) scaled by this, four stops. */
#define VEIL_DIFFUSE  16.0

/* The glare that hides the background is itself light: a washed-out sky near
 * a star is bright haze, not a black hole around it. So the visible haze and
 * the mask's local term are ONE quantity, drawn as it is tested:
 *     haze(theta) = E_psf * PSF/theta^2
 * With them equal, a source is only ever hidden where glare of comparable
 * brightness is on screen. (Two earlier versions got this wrong: one
 * saturated the haze, keeping the Sun's glow the same size from Mars to
 * Neptune; one scaled the mask thousands of times above the drawn haze,
 * which cut a black hole around the Sun at Uranus and Neptune.) */

/* dir: camera-relative direction to the source (any length);
 * sdir: unit direction to the star; e: glare scale (0 = off). */
static inline float star_veil_vis(const double dir[3], const double sdir[3],
                                  double e_psf, double e_floor, double lum)
{
    if (e_psf <= 0.0 && e_floor <= 0.0) return 1.0f;
    double L = sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (L <= 0.0) return 1.0f;
    double c = (dir[0]*sdir[0] + dir[1]*sdir[1] + dir[2]*sdir[2]) / L;
    c = c < -1.0 ? -1.0 : (c > 1.0 ? 1.0 : c);
    double th = acos(c) * (180.0 / M_PI);
    if (th < VEIL_CORE_DEG) th = VEIL_CORE_DEG;
    double glare = e_psf * VEIL_PSF / (th * th) + e_floor * VEIL_FLOOR;
    double r = log2((lum > 1e-6 ? lum : 1e-6) / glare);
    double u = (r - VEIL_LO) / (VEIL_HI - VEIL_LO);
    u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
    return (float)(u * u * (3.0 - 2.0 * u));
}
