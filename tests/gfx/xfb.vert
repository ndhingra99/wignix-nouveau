#version 450
layout(xfb_buffer = 0, xfb_offset = 0, xfb_stride = 16, location = 0) out vec4 cap;
void main()
{
	const vec2 p[3] = vec2[3](vec2(-1, -1), vec2(1, -1), vec2(-1, 1));
	gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
	cap = vec4(p[gl_VertexIndex], gl_VertexIndex, 7.0);
}
