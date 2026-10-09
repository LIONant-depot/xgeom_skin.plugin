
#include "xgeom_skin_mb_clusters.vert"
#include "xgeom_skin_mb_skinning.vert"
#include "mb_input_definition_position.vert"

//
// Vertex inputs - position-only pass (e.g. shadow map), stream 0 only. Still needs the packed
// bone offset/weight fields: a skinned vertex's position is meaningless without them, even here.
//
layout(location = 0) in ivec3 in_Pos;          // xyz = compressed pos (read as R16G16B16A16_SINT, w = m_Packed[0..1], unused)
// m_Packed is read as 16 bit words at offsets 6 and 8 of the vertex: D3D12 (Mesa's Vulkan driver under WSL) needs every attribute's offset
// to be a multiple of min(4, its size), so the 4 bytes at offset 6 it used to read as R8G8B8A8_UINT made the pipeline fail.
layout(location = 1) in uint  in_PackedLo;     // m_Packed[0..1] (R16_UINT)
layout(location = 2) in uvec2 in_PackedHi;     // m_Packed[2..5] (R16G16_UINT)

//
// Gets the vertex local position, deformed by skinning
//
mb_position getVertexLocalPosition()
{
    // Select cluster by push constant index
    ClusterData selectedCluster = cluster[push.clusterIndex];
    const uint  baseBoneIndex   = uint(selectedCluster.posScale.w);

    // Decode compressed position from int16 to [-1,1], then to local (bind pose) space
    const vec3 norm_pos = (vec3(in_Pos) + 32768.0) / 32767.5 - 1.0;
    const vec4 bindPos  = vec4(norm_pos.xyz * selectedCluster.posScale.xyz + selectedCluster.posTranslation.xyz, 1.0);

    const uint lo32 = in_PackedLo | (in_PackedHi.x << 16);
    const mat4 Skin = ComputeSkinMatrix(lo32, in_PackedHi.y, baseBoneIndex, push.maxInfluences);

    mb_position Position;
    Position.Value = Skin * bindPos;

    return Position;
}
