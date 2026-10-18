#ifndef XGEOM_SKIN_EDITOR_INSPECTOR_H
#define XGEOM_SKIN_EDITOR_INSPECTOR_H
#pragma once

// What the Skin Geom editor shows about the compiled geometry (next to the rendering settings, as the Static Geom editor does): the read-only facts of the
// final resource: its size and counts, the skeleton and the animations it is skinned to, how its vertices are skinned, where its memory goes, the vertex
// streams, the meshes down to the clusters, the materials. The mesh tree and the number formatting are shared (xeditor_geom_facts.h).
#include "source/Tools/Editor/xeditor_geom_facts.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin_xgpu_rsc_loader.h"
#include "plugins/xskeleton.plugin/source/xskeleton_xgpu_rsc_loader.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace xgeom_skin_editor
{
    struct skin_geom_inspector
    {
        using mesh           = xeditor::geom_facts::mesh;
        using stream         = xeditor::geom_facts::stream;

        void Update(xrsc::geom_skin& GeomRef, const std::wstring& ResourcePath)
        {
            clear();

            auto& Geom = *xresource::g_Mgr.getResource(GeomRef);
            m_RscGeom.m_Instance = xresource::g_Mgr.getFullGuid(GeomRef).m_Instance;
            m_Skeleton           = Geom.getSkeletonRef();
            m_Animation          = Geom.getAnimationRef();

            std::error_code Ec;
            m_FileSize = static_cast<int>(std::filesystem::file_size(ResourcePath, Ec));

            m_nClusters  = static_cast<int>(Geom.m_nClusters);
            m_nVertices  = static_cast<int>(Geom.m_nVertices);
            m_nFaces     = static_cast<int>(Geom.m_nIndices / 3);
            m_BBox       = Geom.m_BBox;
            m_nMeshes    = Geom.m_nMeshes;
            m_nLODs      = Geom.m_nLODs;
            m_nSubmeshes = Geom.m_nSubMeshs;
            m_nIndices   = static_cast<int>(Geom.m_nIndices);

            // Where the memory goes (the GPU's buffers)
            m_BytesVertices = static_cast<int>(Geom.getVertices().size_bytes());
            m_BytesExtras   = static_cast<int>(Geom.getVertexExtras().size_bytes());
            m_BytesIndices  = static_cast<int>(Geom.getIndices().size_bytes());
            m_BytesClusters = static_cast<int>(Geom.getClusterData().size_bytes());

            // The skinning: the bones of the skeleton, the bones the vertices use (the cluster's base bone + the vertex's offset), how many bones move a vertex
            int nBones = 0;
            if (auto* pSkeleton = xresource::g_Mgr.getResource(m_Skeleton)) nBones = static_cast<int>(pSkeleton->getBones().size());
            m_nBones = nBones;

            float MinBase = 0, MaxBase = 0;
            bool  bFirst  = true;
            for (auto& C : Geom.getClusterData())
            {
                const float Base = C.m_PosScaleAndBaseBoneIndex.m_W;
                MinBase = bFirst ? Base : std::min(MinBase, Base);
                MaxBase = bFirst ? Base : std::max(MaxBase, Base);
                bFirst  = false;
            }
            m_MinBaseBone = static_cast<int>(MinBase);
            m_MaxBaseBone = static_cast<int>(MaxBase);

            std::vector<bool> Used(static_cast<std::size_t>(std::max(nBones, 1)), false);
            std::uint64_t     Influences = 0;
            for (auto& Cluster : Geom.getClusters())
            {
                const int Base = static_cast<int>(Geom.getClusterData()[&Cluster - Geom.getClusters().data()].m_PosScaleAndBaseBoneIndex.m_W);
                for (std::uint32_t i = 0; i < Cluster.m_nVertices; ++i)
                {
                    const auto& V = Geom.getVertices()[Cluster.m_iVertex + i];
                    const auto  W = V.getWeights();
                    int         n = 0;
                    for (int j = 0; j < 4; ++j)
                    {
                        if (W[j] <= 0.0005f) continue;
                        ++n;
                        const int Bone = Base + V.getBoneOffset(j);
                        if (Bone >= 0 && Bone < nBones) Used[Bone] = true;
                    }
                    n = std::max(n, 1);
                    m_MaxInfluences = std::max(m_MaxInfluences, n);
                    Influences     += n;
                }
            }
            m_nBonesUsed        = static_cast<int>(std::count(Used.begin(), Used.end(), true));
            m_AverageInfluences = Geom.m_nVertices ? static_cast<float>(static_cast<double>(Influences) / Geom.m_nVertices) : 0.0f;

            m_VertexStreams.resize(2);
            m_VertexStreams[0].m_Name = "Position and skin";
            m_VertexStreams[0].m_Elements.resize(3);
            m_VertexStreams[0].m_Elements[0] = { "SINT16_3D",  "(X,Y,Z) 16 bit normalized positions, decoded with the cluster's scale and translation" };
            m_VertexStreams[0].m_Elements[1] = { "UINT8_4D",   "the 4 bone offsets, 7 bits each: the vertex's bone is the cluster's base bone + the offset" };
            m_VertexStreams[0].m_Elements[2] = { "UINT8_3D",   "the 3 largest weights (8, 7 and 5 bits): the 4th is what is left of 1, and fewer influences cut the rest (the weight LODs)" };

            m_VertexStreams[1].m_Name = "Extras";
            m_VertexStreams[1].m_Elements.resize(3);
            m_VertexStreams[1].m_Elements[0] = { "UINT16_2D",  "(U,V) 16 bit normalized texture coordinates (they can tile)" };
            m_VertexStreams[1].m_Elements[1] = { "UINT16_2D",  "the normal, octahedral encoded in 16 bits per axis" };
            m_VertexStreams[1].m_Elements[2] = { "UINT16_2D",  "the tangent, octahedral encoded (15 bits for Y), the top bit of Y is the sign of the binormal" };

            for (auto& M : Geom.getDefaultMaterialInstances())
                m_Materials.emplace_back(xresource::g_Mgr.getFullGuid(M).m_Instance);

            xeditor::geom_facts::FillMeshes(Geom, m_Mesh, m_Size);
        }

        void clear()
        {
            m_Mesh.clear();
            m_nClusters = m_nVertices = m_nFaces = 0;
            m_nMeshes = m_nLODs = m_nSubmeshes = m_nIndices = 0;
            m_BytesVertices = m_BytesExtras = m_BytesIndices = m_BytesClusters = 0;
            m_nBones = m_nBonesUsed = m_MinBaseBone = m_MaxBaseBone = m_MaxInfluences = 0;
            m_AverageInfluences = 0;
            m_Skeleton.clear();
            m_Animation.clear();
            m_VertexStreams.clear();
            m_Materials.clear();
        }

        xrsc::geom_skin                             m_RscGeom;
        xrsc::skeleton                              m_Skeleton;
        xrsc::anim_package                          m_Animation;
        std::vector<mesh>                           m_Mesh;
        int                                         m_nClusters = 0, m_nVertices = 0, m_nFaces = 0;
        std::vector<xrsc::material_instance_ref>    m_Materials;
        xmath::fbbox                                m_BBox;
        xmath::fvec3                                m_Size;
        int                                         m_FileSize = 0;
        int                                         m_nMeshes = 0, m_nLODs = 0, m_nSubmeshes = 0, m_nIndices = 0;
        int                                         m_BytesVertices = 0, m_BytesExtras = 0, m_BytesIndices = 0, m_BytesClusters = 0;
        int                                         m_nBones = 0, m_nBonesUsed = 0, m_MinBaseBone = 0, m_MaxBaseBone = 0, m_MaxInfluences = 0;
        float                                       m_AverageInfluences = 0;
        std::vector<stream>                         m_VertexStreams;

        template<typename T>
        static std::string Count(const T Value) { return xeditor::geom_facts::FormatNumber(static_cast<double>(Value), 0); }
        static std::string Bytes(const int Value) { return xeditor::geom_facts::FormatNumber(Value, 0) + " Bytes"; }

        XPROPERTY_DEF
        ( "SkinGeom Inspector", skin_geom_inspector
        , obj_member<"RscGeom",         &skin_geom_inspector::m_RscGeom, member_flags<flags::SHOW_READONLY>, member_help<"Reference to the skin geometry resource">>
        , obj_member<"Skeleton",        &skin_geom_inspector::m_Skeleton, member_flags<flags::SHOW_READONLY>, member_help<"The skeleton this geometry is skinned to (compiled with the bind poses it was made for)">>
        , obj_member<"Animations",      &skin_geom_inspector::m_Animation, member_flags<flags::SHOW_READONLY>, member_help<"The animation package made with the geometry (empty when it has none, or when the skin does not import them)">>
        , obj_scope< "Size"
            , obj_member < "X", +[](skin_geom_inspector& O, bool bRead, std::string& Val) { if (bRead) Val = xeditor::geom_facts::FormatDistance(O.m_Size.m_X); }, member_help<"Width of the geometry bounding box">>
            , obj_member < "Y", +[](skin_geom_inspector& O, bool bRead, std::string& Val) { if (bRead) Val = xeditor::geom_facts::FormatDistance(O.m_Size.m_Y); }, member_help<"Height of the geometry bounding box">>
            , obj_member < "Z", +[](skin_geom_inspector& O, bool bRead, std::string& Val) { if (bRead) Val = xeditor::geom_facts::FormatDistance(O.m_Size.m_Z); }, member_help<"Depth of the geometry bounding box">>
            , member_flags<flags::SHOW_READONLY>
            >
        , obj_member<"nClusters", +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Count(O.m_nClusters); }, member_flags<flags::SHOW_READONLY>, member_help<"Number of clusters (renderable chunks, each with its base bone) in the geometry">>
        , obj_member<"nVertices", +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Count(O.m_nVertices); }, member_flags<flags::SHOW_READONLY>, member_help<"Total number of vertices across all LODs">>
        , obj_member<"nFaces",    +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Count(O.m_nFaces); },    member_flags<flags::SHOW_READONLY>, member_help<"Total number of faces (triangles) across all LODs">>
        , obj_member<"FileSize",  +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Bytes(O.m_FileSize); }, member_flags<flags::SHOW_READONLY>, member_help<"Size of the compiled geometry file on disk">>
        , obj_member<"BBox",      &skin_geom_inspector::m_BBox, member_flags<flags::SHOW_READONLY>, member_ui_open<false>, member_help<"Axis-aligned bounding box of the whole geometry (the decode space of its 16 bit positions)">>
        , obj_scope< "Counts"
            , obj_member<"nMeshes",    +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Count(O.m_nMeshes); },    member_help<"Meshes in the geometry">>
            , obj_member<"nLODs",      +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Count(O.m_nLODs); },      member_help<"Level of detail versions across the meshes">>
            , obj_member<"nSubmeshes", +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Count(O.m_nSubmeshes); }, member_help<"Submeshes (a mesh's part with one material) across the LODs">>
            , obj_member<"nIndices",   +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Count(O.m_nIndices); },   member_help<"Indices of the 16 bit index buffer (3 per face)">>
            , member_flags<flags::SHOW_READONLY>
            >
        , obj_scope< "Skinning"
            , obj_member<"nBones",          +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Count(O.m_nBones); },     member_help<"Bones of the skeleton it is skinned to (0: the skeleton could not be loaded)">>
            , obj_member<"nBonesUsed",      +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Count(O.m_nBonesUsed); }, member_help<"Bones that move at least one vertex">>
            , obj_member<"MaxInfluences",   +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Count(O.m_MaxInfluences); }, member_help<"The most bones that move one vertex (4 at most)">>
            , obj_member<"AverageInfluences", +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = xeditor::geom_facts::FormatNumber(O.m_AverageInfluences, 2); }, member_help<"Bones per vertex on average: what the weight LODs and the vertex cost follow">>
            , obj_member<"BaseBones",       +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Count(O.m_MinBaseBone) + " .. " + Count(O.m_MaxBaseBone); }, member_help<"The range of the clusters' base bone: a vertex's bone is its cluster's base bone plus a 7 bit offset, so one cluster reaches 128 bones from its base">>
            , member_flags<flags::SHOW_READONLY>
            >
        , obj_scope< "Memory"
            , obj_member<"PositionsAndSkin", +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Bytes(O.m_BytesVertices); }, member_help<"The position stream with the skin data: 12 bytes per vertex">>
            , obj_member<"Extras",           +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Bytes(O.m_BytesExtras); },   member_help<"The extras stream: UV, normal and tangent per vertex (12 bytes)">>
            , obj_member<"Indices",          +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Bytes(O.m_BytesIndices); },  member_help<"The index buffer">>
            , obj_member<"ClusterData",      +[](skin_geom_inspector& O, bool bRead, std::string& Value) { if (bRead) Value = Bytes(O.m_BytesClusters); }, member_help<"Per cluster: the scale and translation that decode its positions and UVs, and its base bone">>
            , member_flags<flags::SHOW_READONLY>
            >
        , obj_member<"Meshes",          &skin_geom_inspector::m_Mesh,          member_flags<flags::SHOW_READONLY>, member_help<"List of meshes in the geometry, each containing multiple LODs">>
        , obj_member<"VertexStreams",   &skin_geom_inspector::m_VertexStreams, member_flags<flags::SHOW_READONLY>, member_help<"Vertex data layout: the position stream with the skin data, and the Extras stream (UV, normals, tangents)">>
        , obj_member<"Materials",       &skin_geom_inspector::m_Materials,     member_flags<flags::SHOW_READONLY>, member_help<"References to material instances used by this geometry">>
        );
    };
    XPROPERTY_REG(skin_geom_inspector)
}

#endif // XGEOM_SKIN_EDITOR_INSPECTOR_H
