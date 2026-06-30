#version 330 core
/*
 * bloom_bright.frag — bright-pass extraction for bloom.
 * Keeps only the part of each pixel above u_threshold (soft knee), so only
 * luminous things (star cores, glare, emissive) feed the blur.  The scene is an
 * HDR (RGBA16F) buffer, so additive glare can exceed 1.0 and bloom strongly.
 */
in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform float     u_threshold;

void main() {
    vec3  c   = texture(u_scene, v_uv).rgb;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float k   = clamp((lum - u_threshold) / max(1.0 - u_threshold, 1e-3), 0.0, 1.0);
    frag_color = vec4(c * k, 1.0);
}
