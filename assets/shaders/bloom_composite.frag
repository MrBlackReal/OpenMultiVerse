#version 330 core
/*
 * bloom_composite.frag — final pass to the default framebuffer.
 * Additively blends the blurred bloom over the original scene so the base image
 * is preserved and only luminous areas gain a glow.
 */
in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform float     u_intensity;

void main() {
    vec3 scene = texture(u_scene, v_uv).rgb;
    vec3 bloom = texture(u_bloom, v_uv).rgb;
    frag_color = vec4(scene + bloom * u_intensity, 1.0);
}
