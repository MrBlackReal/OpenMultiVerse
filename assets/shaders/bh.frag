#version 330 core
/*
 * bh.frag — stylized black hole in the Gargantua / EHT idiom.
 *
 * The previous version drew a face-on, concentric "bullseye": a black disk, a
 * ring, and a circular annulus.  A real black hole reads the way it does because
 * the accretion disk is a flat plane seen *at an inclination*, so it shows as a
 * tilted ring of fire — the near edge sweeping in front of the bottom of the
 * dark sphere, the far edge gravitationally lensed up into a bright arc over the
 * top.  We fake that here without ray-marching: compress the disk's vertical
 * axis to tilt it toward the viewer, let the opaque shadow occlude the far
 * (top) half so it only peeks out as the over-the-top arc, and draw the near
 * (bottom) half in front of the shadow.  Relativistic Doppler beaming makes the
 * approaching side brighter and bluer.
 *
 * v_uv is in horizon-radius units (length 1 = event horizon).  Alpha-blended
 * (GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA): the shadow is opaque black so it
 * occludes the background, while the ring/disk write HDR-bright colour that the
 * bloom pass (#2) turns into glow.
 */
in  vec2 v_uv;
out vec4 frag_color;

uniform vec3 u_color;   /* accretion-disk base (warm) colour */

const float HORIZON  = 1.0;    /* event-horizon (shadow) radius            */
const float SQUASH   = 0.28;   /* disk inclination: vertical compression   */
const float DISK_IN  = 2.1;    /* inner disk radius, in disk-plane units   */
const float DISK_OUT = 5.4;    /* outer disk radius                        */

/* Cheap procedural banding so the disk has turbulent structure to bloom. */
float bands(float a, float rd) {
    return 0.80
         + 0.12 * sin(a * 7.0  + rd * 3.0)
         + 0.08 * sin(a * 13.0 - rd * 5.0);
}

void main() {
    float r        = length(v_uv);
    bool  inShadow = (r < HORIZON);

    vec3  col   = vec3(0.0);
    float alpha = inShadow ? 1.0 : 0.0;   /* shadow is opaque to occlude bg */

    /* ---- photon ring: thin, very bright, hugs the shadow -------------- */
    if (!inShadow) {
        float ring = exp(-pow((r - HORIZON * 1.04) / 0.06, 2.0)) * 3.0;
        col   += vec3(1.0, 0.96, 0.88) * ring;
        alpha += ring;
    }

    /* ---- accretion disk: inclined elliptical annulus ----------------- */
    /* Compress y so a circular annulus in disk space projects to a wide,
     * shallow ellipse on screen — the disk tilted toward the camera.     */
    vec2  d   = vec2(v_uv.x, v_uv.y / SQUASH);
    float rd  = length(d);
    float ang = atan(d.y, d.x);

    if (rd > DISK_IN && rd < DISK_OUT) {
        /* The near (bottom, v_uv.y < 0) half crosses in front of the
         * shadow; the far (top) half is behind the hole and only shows
         * where it lenses out past the horizon — i.e. outside the shadow. */
        bool visible = (!inShadow) || (v_uv.y < 0.0);
        if (visible) {
            float t    = (rd - DISK_IN) / (DISK_OUT - DISK_IN);  /* 0..1 out */
            float prof = pow(1.0 - t, 1.6);                      /* hot inner */
            float disk = prof * bands(ang, rd);

            /* Relativistic Doppler beaming: the side spinning toward us
             * (screen-left, ang ~ pi) beams brighter; receding side dims. */
            float beam = 0.40 + 0.60 * (0.5 - 0.5 * cos(ang));
            disk *= beam;

            /* Hot blue-white inner edge -> warm base colour outward, with
             * the approaching side pushed bluer (relativistic blueshift). */
            vec3 hot  = mix(u_color, vec3(0.85, 0.92, 1.0), beam);
            vec3 dcol = mix(hot, u_color, t);

            col   += dcol * disk * 2.4;
            alpha += disk;
        }
    }

    alpha = clamp(alpha, 0.0, 1.0);
    if (alpha < 0.004) discard;
    frag_color = vec4(col, alpha);
}
