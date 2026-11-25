#version 450
// one triangle over the whole target at depth pc.z; t = 4 units per pixel in x
layout(location = 0) out float t;
layout(push_constant) uniform PC { float z; float w; } pc;
void main()
{
	vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
	vec2 n = p * 2.0 - 1.0;
	gl_Position = vec4(n, pc.z, 1.0);
	t = (n.x * 0.5 + 0.5) * pc.w * 4.0;
}
