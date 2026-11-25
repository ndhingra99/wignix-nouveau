#version 450
// Z7: every vertex records its gl_VertexIndex
layout(set = 0, binding = 0) buffer R { uint cnt; uint o[]; };
void main()
{
	uint s = atomicAdd(cnt, 1u);
	o[s] = uint(gl_VertexIndex);
	gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
	gl_PointSize = 1.0;
}
