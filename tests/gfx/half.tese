#version 450
layout(triangles, equal_spacing, ccw) in;
layout(location = 0) out vec4 col;
void main()
{
	gl_Position = gl_TessCoord.x * gl_in[0].gl_Position +
		      gl_TessCoord.y * gl_in[1].gl_Position +
		      gl_TessCoord.z * gl_in[2].gl_Position;
	col = vec4(1, 0, 0, 1);
}
