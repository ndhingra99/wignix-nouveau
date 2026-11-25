#version 450
// Z4b: counts fragment shader invocations
layout(set = 0, binding = 0) buffer C { uint n; };
layout(location = 0) out vec4 c;
void main()
{
	atomicAdd(n, 1u);
	c = vec4(1.0);
}
