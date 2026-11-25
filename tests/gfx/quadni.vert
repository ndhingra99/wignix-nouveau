#version 450
layout(location = 0) out vec4 col;
void main()
{
	const int map[6] = int[6](0, 1, 2, 2, 1, 3);
	int vi = map[gl_VertexIndex];
	vec2 corner = vec2(vi & 1, (vi >> 1) & 1);
	vec2 org = vec2(gl_InstanceIndex & 1, (gl_InstanceIndex >> 1) & 1);
	vec2 px = org * 32.0 + 4.0 + corner * 16.0;
	gl_Position = vec4(px / 32.0 - 1.0, 0.0, 1.0);
	col = vec4(0, 1, 1, 1);
}
