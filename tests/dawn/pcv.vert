#version 450
// diagnosis: the vertex stage stores its push constants
layout(push_constant) uniform PC { float z; float w; } pc;
layout(set = 0, binding = 0) buffer R { vec2 r; };
void main()
{
	r = vec2(pc.z, pc.w);
	gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
	gl_PointSize = 1.0;
}
