#ifndef XGEOM_SKIN_EDITOR_PREVIEW_H
#define XGEOM_SKIN_EDITOR_PREVIEW_H
#pragma once

// The Skin Geom editor's 3D preview: the skinned geometry lit and deformed by the pose of its skeleton (rest pose, bind pose, or an animation
// playing), with the ground grid receiving its shadow. The light's view is drawn into the shadow map first, with a render pass on the window
// before the frame's UI is rendered; the geometry is then drawn from the preview panel's render callback.
#include "plugins/xskeleton.plugin/source/Editor/xskeleton_editor_scene.h"
#include "plugins/xmaterial.plugin/source/xmaterial_xgpu_rsc_loader.h"
#include "plugins/xmaterial.plugin/source/xmaterial_runtime.h"
#include "plugins/xmaterial_instance.plugin/source/xmaterial_instance_xgpu_rsc_loader.h"
#include "plugins/xmaterial_instance.plugin/source/xmaterial_instance_runtime.h"
#include "plugins/xanim_package.plugin/source/xanim_package.h"
#include "plugins/xanim_package.plugin/source/xanim_package_xgpu_rsc_loader.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin_descriptor.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin_details.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin_xgpu_rsc_loader.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin_xgpu_runtime.h"
#include "dependencies/xproperty/source/xcore/my_properties.h"

namespace xgeom_skin_editor::preview
{
    // The two skin shaders every skin geometry is drawn with (compiled with the plugin), and the one that casts its shadow
    inline constexpr std::uint32_t g_GeomSkinVertShader[] =
    {
        #include "GeomSkinBasicShader_vert.h"
    };
    inline constexpr std::uint32_t g_GeomSkinFragShader[] =
    {
        #include "GeomSkinBasicShader_frag.h"
    };
    inline constexpr std::uint32_t g_ShadowVertShader[] =
    {
        #include "GeomSkinShadowMapCreation_vert.h"
    };
    inline constexpr std::uint32_t g_ShadowFragShader[] =
    {
        #include "GeomSkinShadowMapCreation_frag.h"
    };

    struct alignas(256) ubo_geom_skin_mesh
    {
        xmath::fmat4    m_L2w;
        xmath::fmat4    m_w2C;
        xmath::fmat4    m_w2ShadowT;    // read by the shadow lookup of both skin fragment shaders: a real clip-to-shadow-texture matrix, not zero, or shadowing silently does nothing
    };

    struct alignas(256) ubo_bm_lighting
    {
        xmath::fvec4    m_LightColor;
        xmath::fvec4    m_AmbientLightColor;
        xmath::fvec4    m_wSpaceLightPos;
        xmath::fvec4    m_wSpaceEyePos;
        xmath::fvec4    m_LightParams;
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

        XPROPERTY_DEF
        ( "RenderSettings", render_settings
        , obj_member<"PreviewAnimRef", &render_settings::m_PreviewAnimRef >
        , obj_member<"LOD",            &render_settings::m_iLOD >
        , obj_member<"MaxInfluences",  &render_settings::m_MaxInfluences >
        , obj_member<"Pose Type",      &render_settings::m_PoseType, member_enum_span<pose_type_v> >
        )
    };
    XPROPERTY_REG(render_settings)

    struct runtime : xskeleton_editor::scene
    {
        using geom = xgeom_skin::xgpu::geom;

        xgpu::tools::view           m_LightView;

        xgpu::buffer                m_MeshUBO, m_LightUBO, m_ShadowUBO, m_BoneMatrices;
        xgpu::vertex_descriptor     m_SkinVD, m_ShadowVD;
        xgpu::shader                m_SkinVert;
        xgpu::pipeline              m_SkinPipeline, m_ShadowPipeline;
        xgpu::pipeline_instance     m_DefaultInstance, m_ShadowInstance;

        // One entry per material of the geometry: what a submesh draws with. Rebuilt with the geometry.
        std::vector<xgpu::pipeline_instance>        m_MatInstances;
        std::vector<xgpu::pipeline>                 m_MatPipelines;
        std::vector<xrsc::material_instance_ref>    m_MatRefs;

        std::vector<xmath::fmat4>                   m_SkinMatrices;

        // The bindings every skin pipeline shares: per-draw mesh block, lighting, the cluster table and the bone matrices
        static auto SkinBinds() noexcept
        {
            return std::array
            { xgpu::pipeline::uniform_binds{ .m_BindIndex = 0, .m_Usage = { .m_bVertex   = true }, .m_Type = xgpu::pipeline::uniform_binds::type::UBO_DYNAMIC }
            , xgpu::pipeline::uniform_binds{ .m_BindIndex = 1, .m_Usage = { .m_bFragment = true }, .m_Type = xgpu::pipeline::uniform_binds::type::UBO_DYNAMIC }
            , xgpu::pipeline::uniform_binds{ .m_BindIndex = 0, .m_Usage = { .m_bVertex   = true }, .m_Type = xgpu::pipeline::uniform_binds::type::SSBO_STATIC }
            , xgpu::pipeline::uniform_binds{ .m_BindIndex = 1, .m_Usage = { .m_bVertex   = true }, .m_Type = xgpu::pipeline::uniform_binds::type::SSBO_STATIC }
            };
        }

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

            auto UBO = [&](xgpu::buffer& B, int Size) { return Ok(Device.Create(B, { .m_Type = xgpu::buffer::type::UNIFORM, .m_Usage = xgpu::buffer::setup::usage::CPU_WRITE_GPU_READ, .m_EntryByteSize = Size, .m_EntryCount = 100 })); };
            if (!UBO(m_MeshUBO, sizeof(ubo_geom_skin_mesh)) || !UBO(m_LightUBO, sizeof(ubo_bm_lighting)) || !UBO(m_ShadowUBO, sizeof(ubo_shadow_generation_mesh))) return false;
            if (!Ok(Device.Create(m_BoneMatrices, { .m_Type = xgpu::buffer::type::STORAGE, .m_Usage = xgpu::buffer::setup::usage::CPU_WRITE_GPU_READ, .m_EntryByteSize = sizeof(xmath::fmat4), .m_EntryCount = g_MaxBonesSupported }))) return false;

            // Stream 0 is the fused position + skin data (3 x int16 position, 6 packed bytes read back as a uvec4 and a uint16), stream 1 the extras
            {
                auto Attributes = std::array
                { xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_XPos),                      .m_Format = xgpu::vertex_descriptor::format::SINT16_3D,     .m_iStream = 0 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_Packed),                    .m_Format = xgpu::vertex_descriptor::format::UINT8_4D_UINT, .m_iStream = 0 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_Packed) + 4,                .m_Format = xgpu::vertex_descriptor::format::UINT16_1D,     .m_iStream = 0 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex_extras, m_UV),                 .m_Format = xgpu::vertex_descriptor::format::UINT16_2D,     .m_iStream = 1 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex_extras, m_OctNormal),          .m_Format = xgpu::vertex_descriptor::format::UINT16_2D,     .m_iStream = 1 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex_extras, m_OctTangentX),        .m_Format = xgpu::vertex_descriptor::format::UINT16_1D,     .m_iStream = 1 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex_extras, m_OctTangentY_Sign),   .m_Format = xgpu::vertex_descriptor::format::UINT16_1D,     .m_iStream = 1 }
                };
                if (!Ok(Device.Create(m_SkinVD, xgpu::vertex_descriptor::setup{ .m_bUseStreaming = true, .m_Topology = xgpu::vertex_descriptor::topology::TRIANGLE_LIST, .m_VertexSize = 0, .m_Attributes = Attributes }))) return false;
            }
            // The shadow pass reads stream 0 only
            {
                auto Attributes = std::array
                { xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_XPos),       .m_Format = xgpu::vertex_descriptor::format::SINT16_3D,     .m_iStream = 0 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_Packed),     .m_Format = xgpu::vertex_descriptor::format::UINT8_4D_UINT, .m_iStream = 0 }
                , xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(xgeom_skin::geom::vertex, m_Packed) + 4, .m_Format = xgpu::vertex_descriptor::format::UINT16_1D,     .m_iStream = 0 }
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

            // A submesh with no material of its own is drawn with a white diffuse texture
            if (!Ok(Device.Create(m_SkinVert, ShaderSetup(xgpu::shader::type::bit::VERTEX, g_GeomSkinVertShader)))) return false;
            {
                xgpu::shader Frag;
                if (!Ok(Device.Create(Frag, ShaderSetup(xgpu::shader::type::bit::FRAGMENT, g_GeomSkinFragShader)))) return false;
                auto Shaders  = std::array<const xgpu::shader*, 2>{ &Frag, &m_SkinVert };
                auto Samplers = std::array{ xgpu::pipeline::sampler{} };
                auto Binds    = SkinBinds();
                if (!Ok(Device.Create(m_SkinPipeline, xgpu::pipeline::setup{ .m_VertexDescriptor = m_SkinVD, .m_Shaders = Shaders, .m_PushConstantsSize = sizeof(geom_skin_push_const), .m_UniformBinds = Binds, .m_Samplers = Samplers }))) return false;
                auto Bindings = std::array{ xgpu::pipeline_instance::sampler_binding{ m_White } };
                if (!Ok(Device.Create(m_DefaultInstance, { .m_PipeLine = m_SkinPipeline, .m_SamplersBindings = Bindings }))) return false;
            }

            m_bReady = true;
            return true;
        }

        void ReleaseMaterials() noexcept
        {
            if (m_pDevice)
            {
                for (auto& E : m_MatInstances) xeditor::DestroyGpu(m_pDevice, E);
                for (auto& E : m_MatPipelines) xeditor::DestroyGpu(m_pDevice, E);
            }
            for (auto& E : m_MatRefs) xresource::g_Mgr.ReleaseRef(E);
            m_MatInstances.clear();
            m_MatPipelines.clear();
            m_MatRefs.clear();
        }

        void Release() noexcept
        {
            ReleaseMaterials();
            xeditor::DestroyGpu(m_pDevice, m_DefaultInstance, m_ShadowInstance, m_SkinPipeline, m_ShadowPipeline);
            scene::Release();
        }

        // The pipeline instance of every material of the geometry (after each (re)load of the compiled resource). A material instance builds its
        // pipeline from its own material's shader; one without a material draws with the white texture.
        void RebuildMaterials(geom& Geom) noexcept
        {
            ReleaseMaterials();
            const auto Materials = Geom.getDefaultMaterialInstances();
            m_MatInstances.resize(Materials.size());
            m_MatRefs.resize(Materials.size());
            m_MatPipelines.reserve(Materials.size());

            for (auto& MI : Materials)
            {
                const auto Index = static_cast<int>(&MI - Materials.data());
                xresource::g_Mgr.CloneRef(m_MatRefs[Index], MI);

                xmaterial_instance::rt* pMI  = MI.empty() ? nullptr : xresource::g_Mgr.getResource(m_MatRefs[Index]);
                xmaterial::rt*          pMat = (pMI && !pMI->m_MaterialRef.empty()) ? xresource::g_Mgr.getResource(pMI->m_MaterialRef) : nullptr;

                if (pMI && pMat && pMI->m_nTexturesList > 0)
                {
                    // Slot 0 of every material is the shadow map
                    std::vector<xgpu::pipeline_instance::sampler_binding> Bindings;
                    Bindings.reserve(pMI->m_nTexturesList);
                    for (auto& E : pMI->getTextures())
                    {
                        if (&E == pMI->getTextures().data()) { Bindings.emplace_back(m_ShadowMap); continue; }
                        auto* pTexture = xresource::g_Mgr.getResource(E.m_TexureRef);
                        Bindings.emplace_back(pTexture ? *pTexture : m_White);
                    }

                    std::vector<xgpu::pipeline::sampler> Samplers(pMI->m_nTexturesList);
                    Samplers[0] = xgpu::pipeline::sampler{ .m_AddressMode = std::array{ xgpu::pipeline::sampler::address_mode::CLAMP, xgpu::pipeline::sampler::address_mode::CLAMP, xgpu::pipeline::sampler::address_mode::CLAMP } };

                    // The pipeline is this editor's own: the material keeps a single slot for its pipeline and static geometry already uses it
                    auto& NewPipeline = m_MatPipelines.emplace_back();
                    auto  Shaders     = std::array<const xgpu::shader*, 2>{ &pMat->getShader(), &m_SkinVert };
                    auto  Binds       = SkinBinds();
                    if (!Ok(m_pDevice->Create(NewPipeline, xgpu::pipeline::setup{ .m_VertexDescriptor = m_SkinVD, .m_Shaders = Shaders, .m_PushConstantsSize = sizeof(geom_skin_push_const), .m_UniformBinds = Binds, .m_Samplers = Samplers }))) continue;
                    Ok(m_pDevice->Create(m_MatInstances[Index], { .m_PipeLine = NewPipeline, .m_SamplersBindings = Bindings }));
                }
                else
                {
                    auto Bindings = std::array{ xgpu::pipeline_instance::sampler_binding{ m_White } };
                    Ok(m_pDevice->Create(m_MatInstances[Index], { .m_PipeLine = m_SkinPipeline, .m_SamplersBindings = Bindings }));
                }
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

        // The ground with the shadow on it, then the skinned geometry. From inside the panel's render callback.
        void DrawGeom(xgpu::cmd_buffer& CmdBuffer, geom& Geom, const render_settings& Settings) noexcept
        {
            if (!m_bReady) return;
            DrawGrid(CmdBuffer);
            if (Geom.m_nVertices == 0) return;

            // The geometry is drawn relative to the camera, for precision: its matrices are shifted by the camera position and back
            const auto CameraPos = m_View.getPosition();
            const auto L2w       = xmath::fmat4::fromTranslation(-CameraPos);
            const auto w2C       = m_View.getW2C() * xmath::fmat4::fromTranslation(CameraPos);

            CmdBuffer.setStreamingBuffers({ &Geom.IndexBuffer(), 3 });

            auto& MeshUBO = m_MeshUBO.allocEntry<ubo_geom_skin_mesh>();
            MeshUBO.m_L2w       = L2w;
            MeshUBO.m_w2C       = w2C;
            MeshUBO.m_w2ShadowT = ClipToTextureSpace() * m_ShadowL2C;

            auto& Lighting = m_LightUBO.allocEntry<ubo_bm_lighting>();
            Lighting.m_LightColor        = xmath::fvec4(1) * 4;
            Lighting.m_AmbientLightColor = xmath::fvec4(1) * 0.7f;
            Lighting.m_wSpaceLightPos    = xmath::fvec4(m_LightView.getPosition() - CameraPos, Geom.m_BBox.getRadius() * 5);
            Lighting.m_wSpaceEyePos      = xmath::fvec4(0);
            Lighting.m_LightParams.m_X   = Lighting.m_wSpaceLightPos.m_W * 0.1f;
            Lighting.m_LightParams.m_Y   = 6500;      // temperature
            Lighting.m_LightParams.m_Z   = 1;         // intensity boost
            Lighting.m_LightParams.m_W   = 0;

            const std::uint32_t MaxInfluences = static_cast<std::uint32_t>(std::clamp(Settings.m_MaxInfluences, 1, 4));
            std::array StaticSSBO{ &Geom.ClusterBuffer(), &m_BoneMatrices };
            for (auto& M : Geom.getMeshes())
            {
                const int UseLOD = std::clamp(Settings.m_iLOD, 0, int(M.m_nLODs) - 1);
                auto&     L      = Geom.getLODs()[M.m_iLOD + UseLOD];
                for (auto& S : Geom.getSubmeshes().subspan(L.m_iSubmesh, L.m_nSubmesh))
                {
                    auto& Instance = (S.m_iMaterial < m_MatInstances.size() && m_MatInstances[S.m_iMaterial].m_Private) ? m_MatInstances[S.m_iMaterial] : m_DefaultInstance;
                    CmdBuffer.setPipelineInstance(Instance, StaticSSBO);
                    CmdBuffer.setDynamicUBO(m_MeshUBO, 0);
                    CmdBuffer.setDynamicUBO(m_LightUBO, 1);

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
    };
}

#endif // XGEOM_SKIN_EDITOR_PREVIEW_H
