#version 450
#extension GL_EXT_multiview : require
layout(location = 0) out vec4 c;
void main() { c = gl_ViewIndex == 0 ? vec4(1, 0, 0, 1) : vec4(0, 1, 0, 1); }
