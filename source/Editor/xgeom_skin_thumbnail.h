#ifndef XGEOM_SKIN_THUMBNAIL_H
#define XGEOM_SKIN_THUMBNAIL_H
#pragma once

// Skin Geom's thumbnail renderer: the SAME lit-mesh runtime the interactive editor preview panel
// already uses (xgeom_skin_editor::preview::runtime - shading/materials/shadow, all reused as-is,
// not reimplemented), just pointed at a fixed front-biased angle with an exact-fit camera distance
// instead of the panel's own orbit camera - see xgeom_static_thumbnail.h (the sibling this mirrors)
// and xeditor_thumbnail_camera_fit.h for why.
//
// Posed with the skeleton's own REST pose ("zero pose" - xgpu::tools::editors::ComputeRestBoneWorlds,
// the same call EvaluatePose() falls back to when nothing is playing), not the atomic BIND_POSE
// shortcut (all-identity bone matrices): now that xgeom_skin::geom carries a real m_SkeletonRef (see
// that field's own comment - the gap this depended on, fixed alongside this file), there is a real
// skeleton to ask, and its rest pose is the more correct "what does this actually look like" shot -
// bind pose and rest pose only coincide when nobody has ever re-posed the rig since the mesh was
// skinned to it, which is not a safe assumption to bake into every thumbnail forever.
//
// Owns its own small offscreen colour+depth render target and render pass (built once in Init(), reused
// every call) and its own deferred GPU readback - see xeditor_thumbnail.h's contract comment for why.
#include "source/Tools/Editor/xeditor_thumbnail.h"
#include "source/Tools/Editor/xeditor_thumbnail_camera_fit.h"
#include "source/Tools/Editor/xeditor_resource_editor.h"
#include "plugins/xgeom_skin.plugin/source/Editor/xgeom_skin_editor_preview.h"

#include <list>
#include <unordered_map>

namespace xgeom_skin_editor
{
    class thumbnail_renderer final : public xeditor::thumbnail_renderer
    {
    public:
        static constexpr float s_CellPixels = 128.0f;   // matches xeditor_thumbnail_cache::s_CellPixels

        bool Init(xgpu::device& Device) noexcept override
        {
            if (m_bReady) return true;
            if (!m_Runtime.Init(Device)) return false;   // always shadow-enabled internally (runtime::Init hardcodes scene::Init(Device, true))

            const int Cell = static_cast<int>(s_CellPixels);
            if (!Ok(Device.Create(m_ColorTarget, { .m_Format = xgpu::texture::format::R8G8B8A8_UNORM, .m_Width = Cell, .m_Height = Cell, .m_isGamma = false }))) return false;
            if (!Ok(Device.Create(m_DepthTarget, { .m_Format = xgpu::texture::format::DEPTH_U16,      .m_Width = Cell, .m_Height = Cell, .m_isGamma = false }))) return false;
            {
                auto Attachments = std::array<xgpu::renderpass::attachment, 2>{ { m_ColorTarget, m_DepthTarget } };
                if (!Ok(Device.Create(m_Pass, { .m_Attachments = Attachments }))) return false;
            }

            m_bReady = true;
            return true;
        }

        // Polled once per tick (see xeditor_thumbnail.h's contract) until it returns true with OutBitmap
        // filled in. Serialized to one guid at a time, same as xgeom_static_thumbnail.h's identical
        // single-in-flight-target reasoning.
        bool Render(xgpu::device& Device, xgpu::window& Window, xresource::full_guid Guid, xbitmap& OutBitmap) noexcept override
        {
            if (!m_bReady) return false;

            if (m_bBusy)
            {
                if (m_BusyGuid != Guid) return false;
                if (!m_bReadbackDone) return false;
                m_bBusy = false;
                if (m_ReadbackWidth != static_cast<int>(s_CellPixels) || m_ReadbackHeight != static_cast<int>(s_CellPixels)) return false;

                OutBitmap.CreateBitmap(static_cast<int>(s_CellPixels), static_cast<int>(s_CellPixels));
                auto Dst = OutBitmap.getMip<xcolori>(0);
                std::memcpy(Dst.data(), m_ReadbackPixels.data(), Dst.size() * sizeof(xcolori));
                return true;
            }

            auto* pGeom = Reference(Guid);
            if (!pGeom) return false;

            // The skeleton it is skinned to, resolved lazily against the SAME guid's cache slot as the
            // geometry (see Reference()'s own comment) - a real reference now that xgeom_skin::geom
            // carries one, instead of the "no way to find it" gap this used to be.
            auto* pSkeleton = ReferenceSkeleton(Guid, pGeom->getSkeletonRef());
            if (!pSkeleton) return false;

            m_Runtime.RebuildMaterials(*pGeom);

            m_Runtime.m_Center   = pGeom->m_BBox.getCenter();
            m_Runtime.m_Radius   = std::max(0.01f, pGeom->m_BBox.getRadius());
            m_Runtime.m_bReframe = false;   // exact-fit distance below replaces the panel's own bounding-sphere auto-fit

            m_Runtime.m_View.setFov(20_xdeg);
            // Front-biased, not a full 3/4 corner view - same convention as xgeom_static_thumbnail.h's
            // own angle (see that file's comment for the full reasoning): mostly-front reads a
            // silhouette better at 128x128 than a strong diagonal does.
            m_Runtime.m_Angles.m_Pitch = -12_xdeg;
            m_Runtime.m_Angles.m_Yaw   =  15_xdeg;
            m_Runtime.m_Target         = m_Runtime.m_Center;
            m_Runtime.m_Distance       = xeditor::ComputeTightFitDistance(m_Runtime.m_View, m_Runtime.m_Angles, m_Runtime.m_Target, pGeom->m_BBox.m_Min, pGeom->m_BBox.m_Max);
            m_Runtime.UpdateView(ImVec2(0, 0), s_CellPixels, s_CellPixels);   // viewport/aspect + LookAt(Distance, Angles, Target) - m_bReframe is false, so no auto-fit override

            // Rest pose ("zero pose") - same call EvaluatePose() itself falls back to when nothing is
            // playing (xgeom_skin_editor.h), just always taken here rather than only as a fallback.
            xgpu::tools::editors::ComputeRestBoneWorlds(*pSkeleton, m_PoseWorlds);
            m_Runtime.UploadBones(*pSkeleton, m_PoseWorlds, preview::pose_type::FROZEN_POSE);

            const preview::render_settings RenderSettings{ .m_iLOD = 0, .m_MaxInfluences = 4, .m_PoseType = preview::pose_type::FROZEN_POSE };
            m_Runtime.RenderShadow(Window, *pGeom, RenderSettings);

            {
                // cmd_buffer's own destructor ends the render pass - see xgeom_static_thumbnail.h's
                // identical comment on why this is scoped to end before the readback below.
                auto CmdBuffer = Window.StartRenderPass(m_Pass);
                m_Runtime.DrawGeom(CmdBuffer, *pGeom, RenderSettings);
            }

            (void)Window.ReadbackTexture(m_ColorTarget, m_ReadbackPixels, m_ReadbackWidth, m_ReadbackHeight, m_bReadbackDone);
            m_bBusy    = true;
            m_BusyGuid = Guid;
            return false;
        }

    private:
        static bool Ok(xgpu::device::error* pErr) noexcept
        {
            if (!pErr) return true;
            std::printf("Skin Geom thumbnail: %s\n", std::string(xgpu::getErrorMsg(pErr)).c_str());
            return false;
        }

        // Keeps the geometry loaded for a little while after its last thumbnail request - mirrors
        // xgeom_static_thumbnail.h's own Reference()/LRU exactly, for the same reason (a render
        // recorded now is only consumed, and read back, by the GPU several frames later).
        xgeom_skin::xgpu::geom* Reference(const xresource::full_guid& Guid) noexcept
        {
            if (auto It = m_Refs.find(Guid); It != m_Refs.end())
            {
                m_Order.remove(Guid);
                m_Order.push_front(Guid);
                return xresource::g_Mgr.getResource(It->second);
            }

            xrsc::geom_skin Ref;
            Ref.m_Instance = Guid.m_Instance;
            auto& Held = m_Refs.emplace(Guid, Ref).first->second;
            m_Order.push_front(Guid);
            while (m_Refs.size() > m_Capacity)
            {
                const auto Oldest = m_Order.back();
                if (auto It = m_Refs.find(Oldest); It != m_Refs.end()) { xresource::g_Mgr.ReleaseRef(It->second); m_Refs.erase(It); }
                if (auto It = m_SkelRefs.find(Oldest); It != m_SkelRefs.end()) { xresource::g_Mgr.ReleaseRef(It->second); m_SkelRefs.erase(It); }
                m_Order.pop_back();
            }
            return xresource::g_Mgr.getResource(Held);
        }

        // Cloned lazily against the OWNING geom's guid (not the skeleton's own guid - several geom
        // thumbnails could legitimately share one skeleton, but each needs its own held ref/lifetime
        // tied to ITS OWN entry in m_Order, same as the geom ref above) the first call that has a real
        // SkeletonRef to clone (empty until the geom itself has actually loaded far enough to report
        // one). Released in lockstep with the geom ref in Reference()'s own eviction loop above.
        xskeleton::skeleton* ReferenceSkeleton(const xresource::full_guid& GeomGuid, xrsc::skeleton SkeletonRef) noexcept
        {
            if (auto It = m_SkelRefs.find(GeomGuid); It != m_SkelRefs.end())
                return xresource::g_Mgr.getResource(It->second);

            if (SkeletonRef.empty()) return nullptr;
            auto& Held = m_SkelRefs.emplace(GeomGuid, SkeletonRef).first->second;
            return xresource::g_Mgr.getResource(Held);
        }

        preview::runtime                                         m_Runtime;
        bool                                                     m_bReady = false;
        std::vector<xmath::fmat4>                                m_PoseWorlds;

        xgpu::texture                                            m_ColorTarget;
        xgpu::texture                                            m_DepthTarget;
        xgpu::renderpass                                         m_Pass;
        bool                                                     m_bBusy          = false;
        xresource::full_guid                                     m_BusyGuid       {};
        std::vector<std::uint32_t>                               m_ReadbackPixels;
        int                                                      m_ReadbackWidth  = 0;
        int                                                      m_ReadbackHeight = 0;
        bool                                                     m_bReadbackDone  = false;

        std::size_t                                              m_Capacity = 20;
        std::unordered_map<xresource::full_guid, xrsc::geom_skin> m_Refs;
        std::unordered_map<xresource::full_guid, xrsc::skeleton>  m_SkelRefs;
        std::list<xresource::full_guid>                          m_Order;
    };
    // Registered next to g_Registration in xgeom_skin_editor.h (this header only defines the class).
}

#endif // XGEOM_SKIN_THUMBNAIL_H
