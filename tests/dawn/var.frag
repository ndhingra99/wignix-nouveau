#version 450
// Z4b: red = where in the pixel the input was interpolated (x)
layout(location = 0) in float t;
layout(location = 0) out vec4 c;
void main()
{
	c = vec4(fract(t * 0.25), 0.0, 0.0, 1.0);
}
