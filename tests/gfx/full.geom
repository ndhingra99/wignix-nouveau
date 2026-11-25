#version 450
layout(points) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) out vec4 col;
void main()
{
	for (int i = 0; i < 3; i++) {
		vec2 p = vec2((i << 1) & 2, i & 2);
		gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
		col = vec4(0, 0, 1, 1);
		EmitVertex();
	}
	EndPrimitive();
}
