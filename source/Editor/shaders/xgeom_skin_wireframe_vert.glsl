#version 450

// The skinned geometry's wire frame: the position-only skin vertex (the shadow's input), through the static wire frame's geometry and
// fragment shaders (xgeom_static_wireframe_geom/frag.glsl)
#include "../../runtime/xgeom_skin_mb_input_position.vert"

layout(set = 2, binding = 0) uniform Mesh
{
    mat4 L2w;           // Local space -> camera-centered small world
    mat4 w2C;           // Small world -> clip space (projection * view)
} mesh;

layout(location = 0) out vec4 vwWorldPosition;
layout(location = 1) out vec4 vClipPosition;

void main()
{
    vwWorldPosition = mesh.L2w * getVertexLocalPosition().Value;
    vClipPosition   = mesh.w2C * vwWorldPosition;
    gl_Position     = vClipPosition;
}
