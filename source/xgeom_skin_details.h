#ifndef XGEOM_SKIN_DETAILS_H
#define XGEOM_SKIN_DETAILS_H
#pragma once

namespace xgeom_skin
{
    struct details
    {
        struct mesh
        {
            std::string                 m_Name          = {};
            std::vector<int>            m_MaterialList  = {};
            int                         m_NumFaces      = 0;
            int                         m_NumUVs        = 0;
            int                         m_NumColors     = 0;
            int                         m_NumNormals    = 0;
            int                         m_NumTangents   = 0;

            XPROPERTY_DEF
            ( "mesh", mesh
            , obj_member<"Name",            &mesh::m_Name >
            , obj_member<"NumMaterials",    &mesh::m_MaterialList >
            , obj_member<"NumFaces",        &mesh::m_NumFaces >
            , obj_member<"NumUVs",          &mesh::m_NumUVs >
            , obj_member<"NumColors",       &mesh::m_NumColors >
            )
        };

        struct node
        {
            std::string                 m_Name;
            std::vector<int>            m_MeshList;
            std::vector<node>           m_Children;

            XPROPERTY_DEF
            ( "node", node
            , obj_member<"Name",        &node::m_Name >
            , obj_member<"Mesh",        &node::m_MeshList >
            , obj_member<"Children",    &node::m_Children >
            )
        };

        using node_path = std::vector<std::string>;

        int findMaterial(std::string_view Name) const noexcept
        {
            for (auto& E : m_MaterialList)
                if (E == Name) return static_cast<int>(&E - m_MaterialList.data());
            return -1;
        }


        static bool CompareNodePaths(const node_path& A, const node_path& B)
        {
            if (A.size() != B.size()) return false;
            if (A.empty() && B.empty()) return false;

            for (int i = 0; i < A.size(); ++i)
            {
                if (A[i] != B[i]) return false;
            }

            return true;
        }

        /*
        std::pair<const node*, const node*> findNode( std::span<const std::string> NodePath, const node* pNode = nullptr, const node* pParent = nullptr ) const noexcept
        {
            if (pNode == nullptr) pNode = &m_RootNode;
            if (pNode->m_Name == NodePath[0] ) return {pNode, pParent};
            if( NodePath.size() == 1) return{};

            for( auto& n : pNode->m_Children )
            {
                auto Pair = findNode(std::span(NodePath).subspan(1), &n, pNode);
                if (Pair.first) return Pair;
            }

            return {};
        }
        */

        std::pair<const node*, const node*> findNode(std::span<const std::string> NodePath) const noexcept
        {
            if ( NodePath.empty() ) return {};
            if (NodePath.size() == 1 && NodePath[0] == m_RootNode.m_Name) return {&m_RootNode, nullptr};
            if (NodePath[0] != m_RootNode.m_Name) return {};

            int Index=1;
            const node* pParent = &m_RootNode;
            do
            {
                bool bFound = false;
                for (const node& Node : pParent->m_Children)
                {
                    if (Node.m_Name == NodePath[Index])
                    {
                        Index++;
                        if (Index == NodePath.size())
                            return{ &Node, pParent };

                        // Otherwise just keep searching
                        pParent = &Node;
                        bFound = true;
                        break;
                    }
                }

                if (bFound==false) break;

            } while( Index < NodePath.size() );

            return {};
        }

        std::pair<const node*, int>  findMesh(std::span<const std::string> NodePath, std::string_view MeshName) const noexcept
        {
            auto Pair = findNode(NodePath);
            if (Pair.first == nullptr) return {};

            for (auto mi : Pair.first->m_MeshList)
            {
                if ( m_MeshList[mi].m_Name == MeshName )
                    return { Pair.first, mi };
            }

            return {};
        }

        int findMesh(std::string_view MeshName) const noexcept
        {
            for (auto& m : m_MeshList)
            {
                if (m.m_Name == MeshName)
                    return static_cast<int>(&m - m_MeshList.data());
            }

            return -1;
        }


        std::vector<mesh>           m_MeshList;
        std::vector<std::string>    m_MaterialList;
        int                         m_NumFaces;
        node                        m_RootNode;
        bool                        m_bHasAnimations = false;       // the model has animation clips (the descriptor's AnimationRef follows it)
        bool                        m_bCompactTree   = false;       // the node tree is too deep for xproperty's paths: RootNode is empty here, the tree follows as the "NodeTree" table

        // xproperty builds each property path in a 256 char buffer, so deep trees are kept out of it
        static constexpr int        compact_depth_v  = 8;

        static int Depth(const node& N) noexcept
        {
            int d = 0;
            for (auto& C : N.m_Children) d = std::max(d, Depth(C));
            return d + 1;
        }

        // Writes (after the xproperty stream) or reads the whole tree as a flat pre-order table: parent index, name, meshes ("m" + comma list)
        xerr StreamCompactTree(xtextfile::stream& File, node& Tree) noexcept
        {
            struct row { int m_Parent; std::string m_Name; std::string m_Meshes; const node* m_pNode; };
            std::vector<row> Rows;

            if (!File.isReading())
            {
                const auto Flatten = [&](auto&& Self, const node& N, int Parent) -> void
                {
                    std::string Meshes = "m";
                    for (auto i : N.m_MeshList) Meshes += (Meshes.size() > 1 ? "," : "") + std::to_string(i);
                    const int Me = static_cast<int>(Rows.size());
                    Rows.push_back({ Parent, N.m_Name, Meshes, &N });
                    for (auto& C : N.m_Children) Self(Self, C, Me);
                };
                Flatten(Flatten, Tree, -1);
            }

            if (auto Err = File.Record("NodeTree"
                , [&](std::size_t& Count, xerr&) { if (File.isReading()) Rows.resize(Count); else Count = Rows.size(); }
                , [&](std::size_t i, xerr& Err)
                {
                    false
                    || (Err = File.Field("Parent", Rows[i].m_Parent))
                    || (Err = File.Field("Name",   Rows[i].m_Name))
                    || (Err = File.Field("Meshes", Rows[i].m_Meshes));
                }); Err) return Err;

            if (File.isReading())
            {
                // Children of each row, then assemble bottom-up
                std::vector<node> Nodes(Rows.size());
                for (std::size_t i = 0; i < Rows.size(); ++i)
                {
                    Nodes[i].m_Name = Rows[i].m_Name;
                    for (std::size_t p = 1; p < Rows[i].m_Meshes.size();)
                    {
                        const auto e = Rows[i].m_Meshes.find(',', p);
                        Nodes[i].m_MeshList.push_back(std::atoi(Rows[i].m_Meshes.c_str() + p));
                        p = (e == std::string::npos) ? Rows[i].m_Meshes.size() : e + 1;
                    }
                }
                for (int i = static_cast<int>(Rows.size()) - 1; i > 0; --i)    // pre-order: a child always comes after its parent
                    Nodes[Rows[i].m_Parent].m_Children.insert(Nodes[Rows[i].m_Parent].m_Children.begin(), std::move(Nodes[i]));
                if (!Nodes.empty()) Tree = std::move(Nodes[0]);
            }
            return {};
        }

        XPROPERTY_DEF
        ( "details", details
        , obj_member<"RootNode",            &details::m_RootNode,       member_flags<flags::SHOW_READONLY> >
        , obj_member<"MeshList",            &details::m_MeshList,       member_flags<flags::SHOW_READONLY> >
        , obj_member<"NumFaces",            &details::m_NumFaces,       member_flags<flags::SHOW_READONLY> >
        , obj_member<"MaterialList",        &details::m_MaterialList,   member_flags<flags::SHOW_READONLY> >
        , obj_member<"HasAnimations",       &details::m_bHasAnimations, member_flags<flags::SHOW_READONLY> >
        , obj_member<"CompactTree",         &details::m_bCompactTree,   member_flags<flags::DONT_SHOW> >
        )
    };
    XPROPERTY_REG(details)
    XPROPERTY_REG2(mesh_, details::mesh)
    XPROPERTY_REG2(node_, details::node)
}

#endif
