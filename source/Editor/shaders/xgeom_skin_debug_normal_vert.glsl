#version 450

// The skinned geometry's tangent / binormal / normal lines: the full skin vertex (the pose's deformed frame), through the static debug
// normal's geometry and fragment shaders (xgeom_static_debug_normal_geom/frag.glsl)
#include "../../runtime/xgeom_skin_mb_input_full.vert"

layout(set = 2, binding = 0) uniform MeshUniforms
{
    mat4 L2C;          // Local -> clip space (projection * view)
    vec4 ScaleFactor;
    vec4 Color;
} mesh;

layout(location = 0) out vec4 out_A;
layout(location = 1) out vec4 out_B;
layout(location = 2) out vec4 out_Color;

void main()
{
    const mb_full_vertex VertData = getVertexData();

    vec3 Binormal = VertData.Tangent.w * cross(VertData.Normal.xyz, VertData.Tangent.xyz);

    vec3 Axis = VertData.Tangent.xyz  * mesh.ScaleFactor.xxx
              + Binormal.xyz          * mesh.ScaleFactor.yyy
              + VertData.Normal.xyz   * mesh.ScaleFactor.zzz;

    float uMaxDepth     = mesh.ScaleFactor.w;
    out_A               = mesh.L2C * VertData.LocalPos;
    vec4 dir_clip       = mesh.L2C * vec4(Axis, 0.0);
    float depth_scale   = min(out_A.w, uMaxDepth);
    out_B               = out_A + normalize(dir_clip) * (30 * depth_scale * 0.001);

    out_Color           = mesh.Color;

    gl_Position         = out_A;
}
