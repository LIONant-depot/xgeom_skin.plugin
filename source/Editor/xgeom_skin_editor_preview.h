#ifndef XGEOM_SKIN_EDITOR_PREVIEW_H
#define XGEOM_SKIN_EDITOR_PREVIEW_H
#pragma once

// The Skin Geom editor's 3D preview: the skinned geometry lit and deformed by the pose of its skeleton (rest pose, bind pose, or an animation
// playing), with the ground grid receiving its shadow. Kept in step with the static geom preview: the light's view goes into the shadow map,
// then xLIONRender draws the skinned geometry (its skin vertex path, the materials' HDR variants, tone mapped) into this runtime's render_view,
// with the grid as the overlay; all before the frame's UI is rendered. The panel then blits the view's output.
#include "plugins/xskeleton.plugin/source/Editor/xskeleton_editor_scene.h"
#include "plugins/xmaterial.plugin/source/xmaterial_xgpu_rsc_loader.h"
#include "plugins/xmaterial.plugin/source/xmaterial_runtime.h"
#include "plugins/xmaterial_instance.plugin/source/xmaterial_instance_xgpu_rsc_loader.h"
#include "plugins/xmaterial_instance.plugin/source/xmaterial_instance_runtime.h"
#include "plugins/xanim_package.plugin/source/xanim_package.h"
#include "plugins/xtexture.plugin/source/xtexture_xgpu_rsc_loader.h"
#include "plugins/xanim_package.plugin/source/xanim_package_xgpu_rsc_loader.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin_descriptor.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin_details.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin_xgpu_rsc_loader.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin_xgpu_runtime.h"
#include "dependencies/xproperty/source/xcore/my_properties.h"
#include "dependencies/xLIONRender/src/xlionrender.h"

namespace xgeom_skin_editor::preview
{
    // The shaders that cast the skinned geometry's shadow (the lit geometry is xLIONRender's skin vertex path)
    inline constexpr std::uint32_t g_ShadowVertShader[] =
    {
        #include "GeomSkinShadowMapCreation_vert.h"
    };
    inline constexpr std::uint32_t g_ShadowFragShader[] =
    {
        #include "GeomSkinShadowMapCreation_frag.h"
    };

    // The wire frame and the tangent / binormal / normal lines: the skinned vertex through Static Geom's geometry and fragment shaders
    inline constexpr std::uint32_t g_WireVertShader[]   = {
        #include "xgeom_skin_wireframe_vert.h"
    };
    inline constexpr std::uint32_t g_WireGeomShader[]   = {
        #include "xgeom_static_wireframe_geom.h"
    };
    inline constexpr std::uint32_t g_WireFragShader[]   = {
        #include "xgeom_static_wireframe_frag.h"
    };
    inline constexpr std::uint32_t g_NormalVertShader[] = {
        #include "xgeom_skin_debug_normal_vert.h"
    };
    inline constexpr std::uint32_t g_NormalGeomShader[] = {
        #include "xgeom_static_debug_normal_geom.h"
    };
    inline constexpr std::uint32_t g_NormalFragShader[] = {
        #include "xgeom_static_debug_normal_frag.h"
    };

    struct alignas(256) ubo_wireframe_mesh
    {
        xmath::fmat4    m_L2w;          // local -> camera-centered world
        xmath::fmat4    m_w2C;          // camera-centered world -> clip
    };
    struct alignas(256) ubo_debug_normal_mesh
    {
        xmath::fmat4    m_L2C;
        xmath::fvec4    m_ScaleFactor;  // the frame axis to draw (x tangent, y binormal, z normal), w the largest depth
        xmath::fvec4    m_Color;
    };

    // The shadow caster only needs the light's local-to-clip matrix
    struct alignas(256) ubo_shadow_generation_mesh
    {
        xmath::fmat4    m_L2C;
    };

    struct geom_skin_push_const
    {
        std::uint32_t   m_ClusterIndex;
        std::uint32_t   m_MaxInfluences;
    };

    // One buffer of bone matrices, sized once and filled every frame
    constexpr int g_MaxBonesSupported = 256;

    // What drives the bones this frame: the skeleton's rest pose, the pose the mesh was skinned in (no deformation at all), or an animation
    enum class pose_type : std::uint8_t { BIND_POSE, FROZEN_POSE, ANIMATION_POSE };
    static constexpr auto pose_type_v = std::array
    { xproperty::settings::enum_item("Bind Pose",      pose_type::BIND_POSE)
    , xproperty::settings::enum_item("Frozen Pose",    pose_type::FROZEN_POSE)
    , xproperty::settings::enum_item("Animation Pose", pose_type::ANIMATION_POSE)
    };

    // View settings: not part of the resource, not saved
    struct render_settings
    {
        xrsc::anim_package  m_PreviewAnimRef = {};
        int                 m_iLOD           = 0;
        int                 m_MaxInfluences  = 4;
        pose_type           m_PoseType       = pose_type::ANIMATION_POSE;
        xrsc::texture_ref   m_Environment    = {};      // a cubemap: lights the geometry instead of the flat fill (empty: the flat fill)
        bool                m_bWireFrame     = false;
        bool                m_bTangents      = false;
        bool                m_bBinormals     = false;
        bool                m_bNormals       = false;
        bool                m_bGridToYMin    = true;
        float               m_GridYMin       = 0;

        XPROPERTY_DEF
        ( "RenderSettings", render_settings
        , obj_member<"PreviewAnimRef", &render_settings::m_PreviewAnimRef >
        , obj_member<"LOD",            &render_settings::m_iLOD >
        , obj_member<"MaxInfluences",  &render_settings::m_MaxInfluences >
        , obj_member<"Pose Type",      &render_settings::m_PoseType, member_enum_span<pose_type_v> >
        , obj_member<"Environment",    &render_settings::m_Environment, member_help<"A cubemap texture (HDR or not) that lights the geometry: the diffuse and the reflections by roughness.\n"
                                                                                    "Empty: the flat fill. A texture that is not a cubemap is ignored.">>
        , obj_scope< "Debug Geometry"
            , obj_member<"WireFrame",    &render_settings::m_bWireFrame, member_help<"Render the skinned geometry's wire frame (the pose's deformation included)">>
            , obj_member<"Tangents",     &render_settings::m_bTangents,  member_help<"Render tangent vectors as debug lines (red)">>
            , obj_member<"Binormals",    &render_settings::m_bBinormals, member_help<"Render binormal vectors as debug lines (green)">>
            , obj_member<"Normals",      &render_settings::m_bNormals,   member_help<"Render normal vectors as debug lines (blue)">>
            >
        , obj_scope< "Grid"
            , obj_member<"SetToGeomYMin", &render_settings::m_bGridToYMin, member_help<"Align the grid plane to the geometry's minimum Y position (ground level)">>
            , obj_member<"GeomYMin",      &render_settings::m_GridYMin, member_flags<flags::SHOW_READONLY>, member_help<"Y coordinate where the grid plane is positioned when SetToGeomYMin is enabled">>
            >
        )
    };
    XPROPERTY_REG(render_settings)

    struct runtime : xskeleton_editor::scene
    {
        using geom = xgeom_skin::xgpu::geom;

        xgpu::tools::view           m_LightView;

        xgpu::buffer                m_ShadowUBO, m_BoneMatrices;
        xgpu::vertex_descriptor     m_ShadowVD;
        xgpu::pipeline              m_ShadowPipeline;
        xgpu::pipeline_instance     m_ShadowInstance;

        // The debug overlays: the wire frame (stream 0 only, like the shadow) and the frame lines (the whole skin vertex)
        xgpu::vertex_descriptor     m_FullVD;
        xgpu::buffer                m_WireUBO, m_NormalUBO;
        xgpu::pipeline              m_WirePipeline, m_NormalPipeline;
        xgpu::pipeline_instance     m_WireInstance, m_NormalInstance;

        // The renderer the lit geometry goes through (one instance per runtime: the preview and each thumbnail renderer own theirs)
        xlionrender::instance                   m_Renderer;
        xlionrender::render_view                m_RenderView;
        xlionrender::mesh_id                    m_Mesh;                 // the geometry, at m_MeshLOD / m_MeshMaxInfluences
        const geom*                             m_pMeshGeom          = nullptr;
        int                                     m_MeshLOD            = -1;
        int                                     m_MeshMaxInfluences  = -1;
        std::vector<xlionrender::material_id>   m_Materials;            // one per material slot of the geometry
        std::vector<xmaterial_instance::held>    m_MatRefs;             // what m_Materials were made of, held (their GPU objects point at it)
        xrsc::texture_ref                       m_EnvRef{};             // the environment cubemap (render_settings::m_Environment), held while lit by it
        std::uint64_t                           m_EnvInstance        = 0;

        std::vector<xmath::fmat4>                   m_SkinMatrices;

        template<std::size_t N>
        static xgpu::shader::setup ShaderSetup(xgpu::shader::type::bit Type, const std::uint32_t (&Code)[N]) noexcept
        {
            return { .m_Type = Type, .m_Sharer = xgpu::shader::setup::raw_data{ std::span{ (std::int32_t*)Code, N } } };
        }

        bool Init(xgpu::device& Device) noexcept
        {
            if (m_bReady) return true;
            if (!scene::Init(Device, true)) return false;
            m_bReady = false;       // not until the skin pipelines exist too

            m_LightView.setFov(50_xdeg);
            m_LightView.setViewport({ 0, 0, m_ShadowMap.getTextureDimensions()[0], m_ShadowMap.getTextureDimensions()[1] });

            if (!Ok(m_Renderer.Init(Device))) return false;
            m_RenderView.m_Background = { 0.45f, 0.45f, 0.45f, 1.0f };     // the grey the preview and the thumbnails always had

            if (!Ok(Device.Create(m_ShadowUBO, { .m_Type = xgpu::buffer::type::UNIFORM, .m_Usage = xgpu::buffer::setup::usage::CPU_WRITE_GPU_READ, .m_EntryByteSize = sizeof(ubo_shadow_generation_mesh), .m_EntryCount = 100 }))) return false;
            if (!Ok(Device.Create(m_BoneMatrices, { .m_Type = xgpu::buffer::type::STORAGE, .m_Usage = xgpu::buffer::setup::usage::CPU_WRITE_GPU_READ, .m_EntryByteSize = sizeof(xmath::fmat4), .m_EntryCount = g_MaxBonesSupported }))) return false;

            // The shadow pass reads stream 0 only (the fused position + skin data: 3 x int16 position, the 6 packed bytes read back as a uint16
            // and two uint16 - formats every driver takes; see xLIONRender's skin vertex descriptor for the whole layout)
            {
                auto Attributes = std::array
                { xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_XPos),       .m_Format = xgpu::vertex_descriptor::format::SINT16_4D,     .m_iStream = 0 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_Packed),     .m_Format = xgpu::vertex_descriptor::format::UINT16_1D,     .m_iStream = 0 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_Packed) + 2, .m_Format = xgpu::vertex_descriptor::format::UINT16_2D,     .m_iStream = 0 }
                };
                if (!Ok(Device.Create(m_ShadowVD, xgpu::vertex_descriptor::setup{ .m_bUseStreaming = true, .m_Topology = xgpu::vertex_descriptor::topology::TRIANGLE_LIST, .m_VertexSize = 0, .m_Attributes = Attributes }))) return false;
            }

            // The light's view of the geometry: depth only, skinned through the same tables as the main pass
            {
                xgpu::shader Frag, Vert;
                if (!Ok(Device.Create(Frag, ShaderSetup(xgpu::shader::type::bit::FRAGMENT, g_ShadowFragShader)))) return false;
                if (!Ok(Device.Create(Vert, ShaderSetup(xgpu::shader::type::bit::VERTEX,   g_ShadowVertShader)))) return false;
                auto Binds = std::array
                { xgpu::pipeline::uniform_binds{ .m_BindIndex = 0, .m_Usage = { .m_bVertex = true }, .m_Type = xgpu::pipeline::uniform_binds::type::UBO_DYNAMIC }
                , xgpu::pipeline::uniform_binds{ .m_BindIndex = 0, .m_Usage = { .m_bVertex = true }, .m_Type = xgpu::pipeline::uniform_binds::type::SSBO_STATIC }
                , xgpu::pipeline::uniform_binds{ .m_BindIndex = 1, .m_Usage = { .m_bVertex = true }, .m_Type = xgpu::pipeline::uniform_binds::type::SSBO_STATIC }
                };
                auto Shaders = std::array<const xgpu::shader*, 2>{ &Frag, &Vert };
                if (!Ok(Device.Create(m_ShadowPipeline, xgpu::pipeline::setup
                    { .m_VertexDescriptor = m_ShadowVD, .m_Shaders = Shaders, .m_PushConstantsSize = sizeof(geom_skin_push_const), .m_UniformBinds = Binds
                    , .m_DepthStencil = { .m_DepthBiasConstantFactor = 1.25f, .m_DepthBiasSlopeFactor = 2.3f, .m_bDepthBiasEnable = true, .m_bDepthClampEnable = true } }))) return false;
                if (!Ok(Device.Create(m_ShadowInstance, { .m_PipeLine = m_ShadowPipeline }))) return false;
            }

            // The wire frame: the shadow's position-only vertex through the static wire frame's geometry shader. The overlay pass depth-tests
            // against the renderer's scene and the wire lies on that surface, so it is pulled toward the camera
            {
                if (!Ok(Device.Create(m_WireUBO,   { .m_Type = xgpu::buffer::type::UNIFORM, .m_Usage = xgpu::buffer::setup::usage::CPU_WRITE_GPU_READ, .m_EntryByteSize = sizeof(ubo_wireframe_mesh),    .m_EntryCount = 100 }))) return false;
                if (!Ok(Device.Create(m_NormalUBO, { .m_Type = xgpu::buffer::type::UNIFORM, .m_Usage = xgpu::buffer::setup::usage::CPU_WRITE_GPU_READ, .m_EntryByteSize = sizeof(ubo_debug_normal_mesh), .m_EntryCount = 100 }))) return false;
                auto Binds = std::array
                { xgpu::pipeline::uniform_binds{ .m_BindIndex = 0, .m_Usage = { .m_bVertex = true }, .m_Type = xgpu::pipeline::uniform_binds::type::UBO_DYNAMIC }
                , xgpu::pipeline::uniform_binds{ .m_BindIndex = 0, .m_Usage = { .m_bVertex = true }, .m_Type = xgpu::pipeline::uniform_binds::type::SSBO_STATIC }
                , xgpu::pipeline::uniform_binds{ .m_BindIndex = 1, .m_Usage = { .m_bVertex = true }, .m_Type = xgpu::pipeline::uniform_binds::type::SSBO_STATIC }
                };
                xgpu::shader Frag, Geom, Vert;
                if (!Ok(Device.Create(Frag, ShaderSetup(xgpu::shader::type::bit::FRAGMENT, g_WireFragShader)))) return false;
                if (!Ok(Device.Create(Geom, ShaderSetup(xgpu::shader::type::bit::GEOMETRY, g_WireGeomShader)))) return false;
                if (!Ok(Device.Create(Vert, ShaderSetup(xgpu::shader::type::bit::VERTEX,   g_WireVertShader)))) return false;
                auto Shaders = std::array<const xgpu::shader*, 3>{ &Frag, &Geom, &Vert };
                xgpu::pipeline::setup Setup{ .m_VertexDescriptor = m_ShadowVD, .m_Shaders = Shaders, .m_PushConstantsSize = sizeof(geom_skin_push_const), .m_UniformBinds = Binds, .m_Blend = xgpu::pipeline::blend::getAlphaOriginal() };
                Setup.m_DepthStencil.m_DepthBiasConstantFactor = -4.0f;
                Setup.m_DepthStencil.m_DepthBiasSlopeFactor    = -2.0f;
                Setup.m_DepthStencil.m_bDepthBiasEnable        = true;
                Setup.m_DepthStencil.m_bDepthWriteEnable       = false;
                if (!Ok(Device.Create(m_WirePipeline, Setup))) return false;
                if (!Ok(Device.Create(m_WireInstance, { .m_PipeLine = m_WirePipeline }))) return false;

                // The frame lines: stream 0 (position + skin) and stream 1 (UV, oct normal, oct tangent)
                auto Attributes = std::array
                { xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_XPos),       .m_Format = xgpu::vertex_descriptor::format::SINT16_4D, .m_iStream = 0 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_Packed),     .m_Format = xgpu::vertex_descriptor::format::UINT16_1D, .m_iStream = 0 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_Packed) + 2, .m_Format = xgpu::vertex_descriptor::format::UINT16_2D, .m_iStream = 0 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = 0,  .m_Format = xgpu::vertex_descriptor::format::UINT16_2D, .m_iStream = 1 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = 4,  .m_Format = xgpu::vertex_descriptor::format::UINT16_2D, .m_iStream = 1 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = 8,  .m_Format = xgpu::vertex_descriptor::format::UINT16_1D, .m_iStream = 1 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = 10, .m_Format = xgpu::vertex_descriptor::format::UINT16_1D, .m_iStream = 1 }
                };
                if (!Ok(Device.Create(m_FullVD, xgpu::vertex_descriptor::setup{ .m_bUseStreaming = true, .m_Topology = xgpu::vertex_descriptor::topology::TRIANGLE_LIST, .m_VertexSize = 0, .m_Attributes = Attributes }))) return false;
                xgpu::shader NFrag, NGeom, NVert;
                if (!Ok(Device.Create(NFrag, ShaderSetup(xgpu::shader::type::bit::FRAGMENT, g_NormalFragShader)))) return false;
                if (!Ok(Device.Create(NGeom, ShaderSetup(xgpu::shader::type::bit::GEOMETRY, g_NormalGeomShader)))) return false;
                if (!Ok(Device.Create(NVert, ShaderSetup(xgpu::shader::type::bit::VERTEX,   g_NormalVertShader)))) return false;
                auto NShaders = std::array<const xgpu::shader*, 3>{ &NFrag, &NVert, &NGeom };
                if (!Ok(Device.Create(m_NormalPipeline, xgpu::pipeline::setup{ .m_VertexDescriptor = m_FullVD, .m_Shaders = NShaders, .m_PushConstantsSize = sizeof(geom_skin_push_const), .m_UniformBinds = Binds, .m_Blend = xgpu::pipeline::blend::getAlphaOriginal() }))) return false;
                if (!Ok(Device.Create(m_NormalInstance, { .m_PipeLine = m_NormalPipeline }))) return false;
            }

            m_bReady = true;
            return true;
        }

        void ReleaseMaterials() noexcept
        {
            for (auto& E : m_Materials) m_Renderer.DestroyMaterial(E);
            for (auto& E : m_MatRefs) E.Release();
            m_Materials.clear();
            m_MatRefs.clear();
            m_Renderer.DestroyMesh(m_Mesh);
            m_pMeshGeom = nullptr;
        }

        void Release() noexcept
        {
            ReleaseMaterials();
            xresource::g_Mgr.ReleaseRef(m_EnvRef);
            m_EnvInstance = 0;
            if (m_bReady) m_Renderer.DestroyView(m_RenderView);
            xeditor::DestroyGpu(m_pDevice, m_ShadowInstance, m_ShadowPipeline, m_WireInstance, m_WirePipeline, m_NormalInstance, m_NormalPipeline);
            scene::Release();
        }

        // The renderer's material of every material slot of the geometry (after each (re)load of the compiled resource): the material's HDR
        // shader with the shadow map in slot 0 and the material instance's textures after it (white where one does not resolve); the
        // renderer's default material (white diffuse) when the slot has no material. The mesh itself is made by Render (it depends on the LOD).
        // Something the renderer's materials were made from compiled again (xresource::mgr::MarkUpdated): Render makes them again
        bool MaterialsStale() const noexcept
        {
            for (const auto& H : m_MatRefs) if (H.isStale()) return true;
            return false;
        }

        void RebuildMaterials(geom& Geom) noexcept
        {
            ReleaseMaterials();
            const auto Materials = Geom.getDefaultMaterialInstances();
            m_Materials.resize(Materials.size());
            m_MatRefs.resize(Materials.size());

            for (auto& MI : Materials)
            {
                const auto Index = static_cast<int>(&MI - Materials.data());
                auto& Held = m_MatRefs[Index];
                xmaterial_instance::rt* pMI  = MI.empty() ? nullptr : Held.Take(MI);
                xmaterial::rt*          pMat = (pMI && !Held.m_Material.empty()) ? xresource::g_Mgr.getResource(Held.m_Material) : nullptr;

                if (pMI && pMat && pMI->m_nTexturesList > 0)
                {
                    std::vector<xgpu::texture*> Textures;
                    Textures.reserve(pMI->m_nTexturesList);
                    for (auto& E : pMI->getTextures())
                    {
                        if (&E == pMI->getTextures().data()) { Textures.push_back(&m_ShadowMap); continue; }
                        auto* pTexture = xresource::g_Mgr.getResource(Held.m_Textures[&E - pMI->getTextures().data()]);
                        Textures.push_back(pTexture ? pTexture : &m_White);
                    }
                    if (Ok(m_Renderer.CreateMaterial(m_Materials[Index], pMat->getShaderHDR(), Textures, pMat->m_Flags.m_bAlpha))) continue;
                }

                const auto Textures = std::array<xgpu::texture*, 2>{ &m_ShadowMap, &m_White };
                (void)Ok(m_Renderer.CreateDefaultMaterial(m_Materials[Index], Textures));
            }
        }

        // Each bone's world matrix times its inverse bind pose is what moves the vertices. The bind pose skips the formula: World * InvBindPose
        // only cancels out when World is exactly the bone's bind-space transform, which the skeleton's separately authored rest pose is not.
        void UploadBones(const xskeleton::skeleton& Skeleton, const std::vector<xmath::fmat4>& World, pose_type Pose) noexcept
        {
            auto Bones = Skeleton.getBones();
            const int nBones = std::min<int>(static_cast<int>(Bones.size()), g_MaxBonesSupported);
            m_SkinMatrices.resize(Bones.size());
            if (Pose == pose_type::BIND_POSE) std::fill(m_SkinMatrices.begin(), m_SkinMatrices.end(), xmath::fmat4::fromIdentity());
            else for (int i = 0; i < static_cast<int>(Bones.size()); ++i) m_SkinMatrices[i] = World[i] * Bones[i].m_InvBindPose;

            (void)m_BoneMatrices.MemoryMap(0, nBones, [&](void* pData) { std::memcpy(pData, m_SkinMatrices.data(), static_cast<std::size_t>(nBones) * sizeof(xmath::fmat4)); });
        }

        // The light's view of the geometry into the shadow map. Opens its own render pass on the window, so it must run before the frame's UI is rendered.
        void RenderShadow(xgpu::window& Window, geom& Geom, const render_settings& Settings) noexcept
        {
            if (!m_bReady) return;

            // A fixed light direction, so the shadow is a stable depth cue while the camera orbits; refit to the geometry every frame
            const float VerticalFov = m_LightView.getFov().m_Value;
            const float HFov        = 2.0f * std::atan(m_LightView.getAspect() * std::tan(VerticalFov * 0.5f));
            const float Distance    = (m_Radius + 0.01f) / std::tan(std::min(VerticalFov, HFov) * 0.5f);
            m_LightView.setNearZ(Distance * 0.1f);
            m_LightView.setFarZ(Distance + m_Radius * 4.0f);
            m_LightView.LookAt(Distance, xmath::radian3(-50_xdeg, 35_xdeg, 0_xdeg), m_Center);
            m_ShadowL2C = m_LightView.getW2C();

            if (Geom.m_nVertices == 0) return;
            auto CmdBuffer = Window.StartRenderPass(m_ShadowPass);
            CmdBuffer.setStreamingBuffers({ &Geom.IndexBuffer(), 2 });      // the index buffer and stream 0

            std::array StaticSSBO{ &Geom.ClusterBuffer(), &m_BoneMatrices };
            const std::uint32_t MaxInfluences = static_cast<std::uint32_t>(std::clamp(Settings.m_MaxInfluences, 1, 4));
            for (auto& M : Geom.getMeshes())
            {
                const int UseLOD = std::clamp(Settings.m_iLOD, 0, int(M.m_nLODs) - 1);
                auto&     L      = Geom.getLODs()[M.m_iLOD + UseLOD];
                for (auto& S : Geom.getSubmeshes().subspan(L.m_iSubmesh, L.m_nSubmesh))
                {
                    CmdBuffer.setPipelineInstance(m_ShadowInstance, StaticSSBO);
                    m_ShadowUBO.allocEntry<ubo_shadow_generation_mesh>().m_L2C = m_ShadowL2C;
                    CmdBuffer.setDynamicUBO(m_ShadowUBO, 0);

                    geom_skin_push_const PushConst{ .m_ClusterIndex = S.m_iCluster, .m_MaxInfluences = MaxInfluences };
                    for (auto& C : Geom.getClusters().subspan(S.m_iCluster, S.m_nCluster))
                    {
                        CmdBuffer.setPushConstants(PushConst);
                        CmdBuffer.Draw(C.m_nIndices, C.m_iIndex, C.m_iVertex);
                        PushConst.m_ClusterIndex++;
                    }
                }
            }
        }

        // The skinned geometry through xLIONRender into m_RenderView (ViewW x ViewH) with the grid over it. Opens render passes on the window:
        // call after RenderShadow, before the frame's UI is rendered; then show m_RenderView.m_Output with m_Renderer.Blit.
        void Render(xgpu::window& Window, geom& Geom, const render_settings& Settings, float ViewW, float ViewH) noexcept
        {
            if (!m_bReady || ViewW <= 1.f || ViewH <= 1.f) return;
            if (MaterialsStale()) RebuildMaterials(Geom);       // a material compiled again

            const int W = static_cast<int>(ViewW), H = static_cast<int>(ViewH);
            if (m_RenderView.m_Width != W || m_RenderView.m_Height != H)
                if (!Ok(m_Renderer.CreateView(m_RenderView, { .m_Width = W, .m_Height = H }))) return;

            // The renderer's mesh follows the geometry, its LOD and how many bone weights are used
            const int MaxInfluences = std::clamp(Settings.m_MaxInfluences, 1, 4);
            if (Geom.m_nVertices && (m_pMeshGeom != &Geom || m_MeshLOD != Settings.m_iLOD || m_MeshMaxInfluences != MaxInfluences))
            {
                m_Renderer.DestroyMesh(m_Mesh);
                if (!Ok(m_Renderer.CreateClusterGeom(m_Mesh, xlionrender::MakeSkinGeom(Geom, m_BoneMatrices, Settings.m_iLOD, MaxInfluences)))) return;
                m_pMeshGeom = &Geom; m_MeshLOD = Settings.m_iLOD; m_MeshMaxInfluences = MaxInfluences;
            }

            // A warm directional key light along the grid shadow's light view (the renderer's shadow clipmap shadows it; its level-0 tiles are
            // sized to the geometry), with a soft fill
            const float Radius = Geom.m_BBox.getRadius();
            xlionrender::scene Scene;
            if (Geom.m_nVertices) Scene.m_Draws.push_back({ .m_Mesh = m_Mesh, .m_Materials = m_Materials });
            Scene.m_Sun     = { .m_Direction = m_Center - m_LightView.getPosition(), .m_Temperature = 6500, .m_Intensity = 4, .m_ShadowTileSize = std::max(Radius * 0.5f, 0.001f) };
            Scene.m_Ambient = { 0.7f, 0.7f, 0.7f };

            // The environment cubemap lights instead of the flat fill (a texture that is not a cubemap: the flat fill)
            if (Settings.m_Environment.m_Instance.m_Value != m_EnvInstance)
            {
                xresource::g_Mgr.ReleaseRef(m_EnvRef);
                m_EnvRef      = Settings.m_Environment;
                m_EnvInstance = Settings.m_Environment.m_Instance.m_Value;
            }
            if (const xgpu::texture* pEnv = m_EnvRef.empty() ? nullptr : xresource::g_Mgr.getResource(m_EnvRef); pEnv && pEnv->isCubemap())
                Scene.m_Sky = { .m_Intensity = 1, .m_pCubemap = pEnv };

            m_Renderer.Render(Window, Scene, m_RenderView, m_View, [&](xgpu::cmd_buffer& CmdBuffer) { DrawOverlays(CmdBuffer, Geom, Settings); });
        }

        // The submeshes of the previewed LOD, each with its push constants (cluster index, bone influences): SetUBO runs per submesh
        template<typename T_FN>
        static void ForEachCluster(geom& Geom, const render_settings& Settings, xgpu::cmd_buffer& CmdBuffer, T_FN&& SetUBO) noexcept
        {
            const std::uint32_t MaxInfluences = static_cast<std::uint32_t>(std::clamp(Settings.m_MaxInfluences, 1, 4));
            for (auto& M : Geom.getMeshes())
            {
                if (M.m_nLODs == 0) continue;
                auto& L = Geom.getLODs()[M.m_iLOD + std::clamp(Settings.m_iLOD, 0, int(M.m_nLODs) - 1)];
                for (auto& S : Geom.getSubmeshes().subspan(L.m_iSubmesh, L.m_nSubmesh))
                {
                    SetUBO();
                    geom_skin_push_const PushConst{ .m_ClusterIndex = S.m_iCluster, .m_MaxInfluences = MaxInfluences };
                    for (auto& C : Geom.getClusters().subspan(S.m_iCluster, S.m_nCluster))
                    {
                        CmdBuffer.setPushConstants(PushConst);
                        CmdBuffer.Draw(C.m_nIndices, C.m_iIndex, C.m_iVertex);
                        PushConst.m_ClusterIndex++;
                    }
                }
            }
        }

        // The wire frame, the grid and the tangent / binormal / normal lines, in display colors over the tone-mapped image
        void DrawOverlays(xgpu::cmd_buffer& CmdBuffer, geom& Geom, const render_settings& Settings) noexcept
        {
            if (!m_bReady) return;

            // Everything is drawn relative to the camera
            const auto L2w = xmath::fmat4::fromTranslation(-m_View.getPosition());
            const auto w2C = m_View.getW2C() * xmath::fmat4::fromTranslation(m_View.getPosition());
            std::array  StaticSSBO{ &Geom.ClusterBuffer(), &m_BoneMatrices };

            if (Geom.m_nVertices && Settings.m_bWireFrame)
            {
                CmdBuffer.setPipelineInstance(m_WireInstance, StaticSSBO);
                CmdBuffer.setStreamingBuffers({ &Geom.IndexBuffer(), 2 });      // the index buffer and stream 0
                ForEachCluster(Geom, Settings, CmdBuffer, [&]
                {
                    auto& UBO = m_WireUBO.allocEntry<ubo_wireframe_mesh>();
                    UBO.m_L2w = L2w;
                    UBO.m_w2C = w2C;
                    CmdBuffer.setDynamicUBO(m_WireUBO, 0);
                });
            }

            // The grid sits at the bottom of the geometry
            xeditor_tools::grid::Draw(CmdBuffer, m_View.getW2C(), m_View.getPosition(), m_ShadowL2C, Settings.m_bGridToYMin ? Geom.m_BBox.m_Min.m_Y : 0.0f);

            if (Geom.m_nVertices)
            {
                static constexpr auto Colors = std::array{ xmath::fvec4{1,0,0,1}, xmath::fvec4{0,1,0,1}, xmath::fvec4{0,0,1,1} };
                static constexpr auto Axis   = std::array{ xmath::fvec4{1,0,0,0}, xmath::fvec4{0,1,0,1}, xmath::fvec4{0,0,1,1} };
                const auto List = std::array{ Settings.m_bTangents, Settings.m_bBinormals, Settings.m_bNormals };
                for (std::size_t i = 0; i < List.size(); ++i)
                {
                    if (!List[i]) continue;
                    CmdBuffer.setPipelineInstance(m_NormalInstance, StaticSSBO);
                    CmdBuffer.setStreamingBuffers({ &Geom.IndexBuffer(), 3 });  // the index buffer, stream 0 and stream 1
                    ForEachCluster(Geom, Settings, CmdBuffer, [&]
                    {
                        auto& UBO = m_NormalUBO.allocEntry<ubo_debug_normal_mesh>();
                        UBO.m_L2C         = w2C * L2w;
                        UBO.m_ScaleFactor = Axis[i];
                        UBO.m_ScaleFactor.m_W = Geom.m_BBox.getRadius();
                        UBO.m_Color       = Colors[i];
                        CmdBuffer.setDynamicUBO(m_NormalUBO, 0);
                    });
                }
            }
        }
    };
}

#endif // XGEOM_SKIN_EDITOR_PREVIEW_H
