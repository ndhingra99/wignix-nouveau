#version 450
// Z2: vertex attribute fetched past the end of its buffer
layout(location = 0) in vec4 a;
layout(set = 0, binding = 0) buffer R { vec4 r[]; };
void main()
{
	r[gl_VertexIndex] = a;
	gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
	gl_PointSize = 1.0;
}
