#version 450
// Z4a: colour from gl_SampleID (needs the SampleRateShading capability)
layout(location = 0) out vec4 c;
void main()
{
	c = vec4(float(gl_SampleID) * 0.25 + 0.1, 0.0, 0.0, 1.0);
}
