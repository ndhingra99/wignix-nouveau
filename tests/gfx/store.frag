#version 450
layout(std430, set = 0, binding = 0) buffer B { uint v[]; };
layout(set = 0, binding = 1, rgba8) uniform image2D img;
layout(location = 0) out vec4 c;
void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	v[p.x + 64 * p.y] = uint(p.x) | (uint(p.y) << 8) | 0xab0000u;
	imageStore(img, p, vec4(p.x & 1, p.y & 1, 0, 1));
	c = vec4(0, 1, 0, 1);
}
