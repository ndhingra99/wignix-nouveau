#version 450
// diagnosis: all 32 push constant words and gl_InstanceIndex, as the vertex
// stage sees them
layout(push_constant) uniform PC { uint v[32]; } pc;
layout(set = 0, binding = 0) buffer R { uint r[]; };
void main()
{
	for (int i = 0; i < 32; i++)
		r[i] = pc.v[i];
	r[32] = uint(gl_InstanceIndex);
	gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
	gl_PointSize = 1.0;
}
