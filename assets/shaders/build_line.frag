#version 330 core
in vec4 v_color;
out vec4 frag_color;

void main() {
    gl_FragDepth = log_depth(1.0 / gl_FragCoord.w);
    frag_color = v_color;
}
