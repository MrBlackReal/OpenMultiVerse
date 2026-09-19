/*
 * starfield.h — catalog-backed starfield rendered as a skybox (rotation only)
 */
#pragma once
#include "common.h"

void starfield_init(void);
/* view_rot: view matrix with translation stripped (rotation only)
 * proj:     projection matrix
 * fade:     0..1 global dimming — the skybox is a direction-only backdrop
 *           for the stellar neighbourhood, so render.c fades it out as the
 *           camera travels to galactic scale (the Milky Way volume takes
 *           over as the unresolved-star glow) */
void starfield_render(const float view_rot[16], const float proj[16],
                      float fade);
void starfield_shutdown(void);
