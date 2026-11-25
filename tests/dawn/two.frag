#version 450
// Z5: the same colour to two attachments
layout(location = 0) out vec4 c0;
layout(location = 1) out vec4 c1;
void main()
{
	c0 = vec4(0.4, 0.4, 0.4, 1.0);
	c1 = vec4(0.4, 0.4, 0.4, 1.0);
}
