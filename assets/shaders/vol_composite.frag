#version 330 core
/*
 * vol_composite.frag — upscale a half-resolution volumetric layer back over the
 * scene. The layer holds premultiplied-alpha colour (rgb already multiplied by
 * coverage), so the caller composites it with premultiplied "over":
 *     glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
 * Bilinear sampling (GL_LINEAR on the source texture) does the upscale.
 */
in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_tex;

void main() {
    frag_color = texture(u_tex, v_uv);
}
