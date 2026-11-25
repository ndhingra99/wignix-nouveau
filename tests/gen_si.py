#!/usr/bin/python3
# Generates the compute shaders of the storage image test (vksi.c): for each
# format and image dimensionality one shader that stores a pattern and one
# that loads it back into a buffer.  usage: gen_si.py <out dir>
import sys
FORMATS = [  # glsl name, kind (f float, n normalized, i sint, u uint)
    ("rgba8", "n"), ("rgba8_snorm", "n"), ("rgba8ui", "u"), ("rgba8i", "i"),
    ("rgba16ui", "u"), ("rgba16i", "i"), ("rgba16f", "f"),
    ("r32ui", "u"), ("r32i", "i"), ("r32f", "f"),
    ("rgba32ui", "u"), ("rgba32i", "i"), ("rgba32f", "f"),
    ("rg16f", "f"), ("r8", "n"), ("rgb10_a2", "n"), ("r11f_g11f_b10f", "f"),
]
DIMS = [("2d", "image2D", "p.xy"), ("2da", "image2DArray", "p"), ("3d", "image3D", "p")]
VAL = {
    "f": ("", "vec4", "vec4(p.x, p.y, p.x + p.y + p.z * 3, 1)", "floatBitsToUint(val)"),
    "n": ("", "vec4", "vec4(p.x & 1, p.y & 1, (p.x ^ p.y ^ p.z) & 1, 1)", "floatBitsToUint(val)"),
    "i": ("i", "ivec4", "ivec4(p.x, -p.y, p.x + p.y + p.z * 3, 1)", "uvec4(val)"),
    "u": ("u", "uvec4", "uvec4(p.x, p.y, p.x + p.y + p.z * 3, 1)", "val"),
}
HEAD = """#version 450
layout(local_size_x = 4, local_size_y = 4) in;
layout(set = 0, binding = 0, %s) uniform %s%s img;
layout(std430, set = 0, binding = 1) buffer B { uvec4 v[]; };
void main()
{
	ivec3 p = ivec3(gl_GlobalInvocationID);
"""
for fmt, kind in FORMATS:
    pfx, vec, value, conv = VAL[kind]
    for dim, img, coord in DIMS:
        base = "%s/%s_%s" % (sys.argv[1], fmt, dim)
        open(base + "_store.comp", "w").write(
            HEAD % (fmt, pfx, img) + "\t%s val = %s;\n\timageStore(img, %s, val);\n}\n" % (vec, value, coord))
        open(base + "_load.comp", "w").write(
            HEAD % (fmt, pfx, img) + "\t%s val = imageLoad(img, %s);\n\tv[p.x + 20 * (p.y + 12 * p.z)] = %s;\n}\n" % (vec, coord, conv))
print(" ".join("%s:%s" % f for f in FORMATS))
