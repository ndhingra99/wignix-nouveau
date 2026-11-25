#version 450
// diagnosis: the fragment stage stores its push constants
layout(push_constant) uniform PC { float z; float w; } pc;
layout(set = 0, binding = 0) buffer R { vec2 r; };
layout(location = 0) out vec4 c;
void main()
{
	r = vec2(pc.z, pc.w);
	c = vec4(1.0);
}
