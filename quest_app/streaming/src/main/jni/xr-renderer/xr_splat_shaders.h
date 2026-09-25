// The splat shaders, GLSL ES 3.00 so the WebGL2 test page (host_tools/worlds/
// splat_harness.py) reads these same strings out of this file and draws with them.
// Every splat is one instance of a four corner strip; its index into the data
// texture arrives as the instanced attribute, already in back to front order.
#ifndef XR_SPLAT_SHADERS_H
#define XR_SPLAT_SHADERS_H

static const char* const SPLAT_VERTEX_SRC =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp usampler2D;\n"
    "layout(location = 0) in uint a_index;\n"
    "uniform usampler2D u_splats;\n"
    "uniform mat4 u_view;\n"
    "uniform mat4 u_proj;\n"
    "uniform vec2 u_focal;\n"      // pixels per unit of tangent, per axis
    "uniform vec2 u_viewport;\n"   // pixels
    "uniform float u_bound;\n"     // how far off the edge a centre may be and still be drawn, 1.2 unless set
    "out vec4 v_color;\n"
    "out vec2 v_pos;\n"
    "void main() {\n"
    "  ivec2 at = ivec2(int(a_index & 2047u) * 2, int(a_index >> 11));\n"
    "  uvec4 a = texelFetch(u_splats, at, 0);\n"
    "  uvec4 b = texelFetch(u_splats, at + ivec2(1, 0), 0);\n"
    "  vec4 cam = u_view * vec4(uintBitsToFloat(a.xyz), 1.0);\n"
    "  vec4 clip = u_proj * cam;\n"
    "  float bound = max(u_bound, 1.2) * clip.w;\n"
    // Behind, too close, or well off the edge: collapsed out of the way
    "  if (cam.z > -0.1 || abs(clip.x) > bound || abs(clip.y) > bound) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }\n"
    "  vec2 c0 = unpackHalf2x16(b.x), c1 = unpackHalf2x16(b.y), c2 = unpackHalf2x16(b.z);\n"
    "  mat3 sigma = uintBitsToFloat(b.w) * mat3(c0.x, c0.y, c1.x, c0.y, c1.y, c2.x, c1.x, c2.x, c2.y);\n"
    // The projection's Jacobian in pixels, the camera looking down -z
    "  float z = cam.z;\n"
    "  mat3 J = mat3(-u_focal.x / z, 0.0, 0.0, 0.0, -u_focal.y / z, 0.0,\n"
    "                u_focal.x * cam.x / (z * z), u_focal.y * cam.y / (z * z), 0.0);\n"
    "  mat3 T = J * mat3(u_view);\n"
    "  mat3 cov = T * sigma * transpose(T);\n"
    // A little blur so a splat never falls between pixels
    "  float ca = cov[0][0] + 0.3, cc = cov[1][1] + 0.3, cb = cov[0][1];\n"
    "  float mid = 0.5 * (ca + cc);\n"
    "  float r = length(vec2(0.5 * (ca - cc), cb));\n"
    "  float l1 = mid + r, l2 = max(mid - r, 0.1);\n"
    "  vec2 axis = abs(cb) > 1e-7 ? normalize(vec2(cb, l1 - ca)) : (ca >= cc ? vec2(1.0, 0.0) : vec2(0.0, 1.0));\n"
    // Three standard deviations each way, and never wider than the eye
    "  float r1 = min(3.0 * sqrt(l1), 1024.0), r2 = min(3.0 * sqrt(l2), 1024.0);\n"
    "  vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;\n"
    "  vec2 offset = corner.x * r1 * axis + corner.y * r2 * vec2(axis.y, -axis.x);\n"
    "  gl_Position = vec4(clip.xy / clip.w + offset * 2.0 / u_viewport, 0.0, 1.0);\n"
    "  uvec4 c = (uvec4(a.w) >> uvec4(0u, 8u, 16u, 24u)) & 255u;\n"
    "  v_color = vec4(c) / 255.0;\n"
    "  v_pos = corner * 3.0;\n"
    "}\n";

// Premultiplied, drawn back to front with ONE, ONE_MINUS_SRC_ALPHA
static const char* const SPLAT_FRAGMENT_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 v_color;\n"
    "in vec2 v_pos;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "  float d2 = dot(v_pos, v_pos);\n"
    "  if (d2 > 9.0) discard;\n"
    "  float alpha = exp(-0.5 * d2) * v_color.a;\n"
    "  if (alpha < 1.0 / 255.0) discard;\n"
    "  fragColor = vec4(v_color.rgb * alpha, alpha);\n"
    "}\n";

#endif
