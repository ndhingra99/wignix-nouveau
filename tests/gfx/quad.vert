#version 450
layout(location = 0) out vec4 col;
void main()
{
	vec2 corner = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1);
	vec2 org = vec2(gl_InstanceIndex & 1, (gl_InstanceIndex >> 1) & 1);
	vec2 px = org * 32.0 + 4.0 + corner * 16.0;
	gl_Position = vec4(px / 32.0 - 1.0, 0.0, 1.0);
	col = vec4(1, 1, 0, 1);
}
