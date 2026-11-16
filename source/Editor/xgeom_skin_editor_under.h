#ifndef XGEOM_SKIN_EDITOR_UNDER_H
#define XGEOM_SKIN_EDITOR_UNDER_H
#pragma once
// The skin drawn UNDER a skeleton view (xskeleton_editor::skeleton_viewport::m_Under): the Skin Geom editor's own preview (lit, shadowed, its
// materials), posed by bone worlds in the skin's space (xgpu::tools::editors::ComputeRestBoneWorlds / ComputeAnimatedBoneWorlds: the bind space,
// with the skeleton's scale), seen by the viewport's camera. One piece every editor that shows a character shares (the Ragdoll editor, the
// Animation Package editor): the same look everywhere.
#include "plugins/xgeom_skin.plugin/source/Editor/xgeom_skin_editor_preview.h"
#include "plugins/xskeleton.plugin/source/Editor/xskeleton_viewport.h"
#include <span>
#include <vector>

namespace xgeom_skin_editor
{
    struct skin_under
    {
        preview::runtime            m_View;
        preview::render_settings    m_Settings;
        const void*                 m_pMaterialsOf  = nullptr;      // the skin the preview's materials were made for
        bool                        m_bInit         = false;
        std::vector<xmath::fmat4>   m_Worlds;

        skin_under() noexcept { m_Settings.m_bGridToYMin = false; }     // the character stands on y = 0, as the bones' grid
        void Release() noexcept { if (m_bInit) m_View.Release(); m_bInit = false; m_pMaterialsOf = nullptr; }
        bool Ready(xgpu::device* pDevice) noexcept { if (!m_bInit && pDevice) m_bInit = m_View.Init(*pDevice); return m_bInit; }

        // Draws Geom posed by Worlds (one per skeleton bone, the skin's space) into the viewport's rectangle; bOpen: the editor is still open
        // when the GPU blits it. Bounds: the posed box (a pose that moved away from the bind box would be culled by it).
        void Render(xgeom_skin::xgpu::geom& Geom, const xskeleton::skeleton& Skel, std::span<const xmath::fmat4> Worlds, const xskeleton_editor::skeleton_viewport& V
                   , const ImVec2& Min, const ImVec2& Avail, const bool& bOpen) noexcept
        {
            auto* pHost   = xeditor::host::current();
            auto* pWindow = pHost ? pHost->find<xgpu::window>() : nullptr;
            if (!pWindow || !m_bInit || Worlds.empty()) return;
            if (m_pMaterialsOf != &Geom) { m_View.RebuildMaterials(Geom); m_pMaterialsOf = &Geom; }     // (as the Skin editor on load)
            m_View.m_View   = V.m_Scene.m_View;
            m_View.m_Center = V.m_Scene.m_Center;
            m_View.m_Radius = V.m_Scene.m_Radius;
            m_Worlds.assign(Worlds.begin(), Worlds.end());
            m_View.UploadBones(Skel, m_Worlds, preview::pose_type::ANIMATION_POSE);
            xmath::fbbox B{ xmath::fvec3(1e30f), xmath::fvec3(-1e30f) };
            for (const auto& W : Worlds) { const auto P = W.ExtractPosition(); B.m_Min = xmath::fvec3::Min(B.m_Min, P); B.m_Max = xmath::fvec3::Max(B.m_Max, P); }
            B.m_Min = B.m_Min - xmath::fvec3(0.3f); B.m_Max = B.m_Max + xmath::fvec3(0.3f);
            m_View.m_PoseBounds = B;
            m_View.RenderShadow(*pWindow, Geom, m_Settings);
            m_View.Render(*pWindow, Geom, m_Settings, Avail.x, Avail.y);
            ImGui::PushClipRect(Min, ImVec2(Min.x + Avail.x, Min.y + Avail.y), true);
            xgpu::tools::imgui::AddCustomRenderCallback([this, &bOpen](xgpu::cmd_buffer& CmdBuffer, const ImVec2&, const ImVec2&)
            {
                if (bOpen) m_View.m_Renderer.Blit(CmdBuffer, m_View.m_RenderView);
            });
            ImGui::PopClipRect();
        }

        // A pose of another skeleton (Src: the bones shown, their worlds in Src's ComputeRestBoneWorlds space) carried onto the skin's own
        // skeleton by bone NAME: each skin bone takes its namesake's change from rest (a bone the source lacks follows its parent), so a skin
        // made on another import of the same character (other helpers, other order, other scale) still follows. Out: the skin's worlds.
        void PoseFrom(const xskeleton::skeleton& Src, std::span<const xmath::fmat4> SrcWorlds, const xskeleton::skeleton& Skin, std::vector<xmath::fmat4>& Out) noexcept
        {
            if (m_pMapSrc != &Src || m_pMapSkin != &Skin)
            {
                m_pMapSrc = &Src; m_pMapSkin = &Skin;
                xgpu::tools::editors::ComputeRestBoneWorlds(Src, m_SrcRest);
                xgpu::tools::editors::ComputeRestBoneWorlds(Skin, m_SkinRest);
                m_SrcInvRest.resize(m_SrcRest.size()); for (std::size_t i = 0; i < m_SrcRest.size(); ++i) m_SrcInvRest[i] = m_SrcRest[i].Inverse();
                const auto SN = Src.getBoneNames(), KN = Skin.getBoneNames();
                m_Map.assign(KN.size(), -1);
                for (std::size_t s = 0; s < KN.size(); ++s)
                    for (std::size_t a = 0; a < SN.size(); ++a) if (SN[a].m_NameHash == KN[s].m_NameHash) { m_Map[s] = int(a); break; }
                int nMiss = 0; for (int v : m_Map) nMiss += v < 0;
                std::printf("[SkinUnder] the skin's %zu bones follow %zu shown bones by name: %d without a namesake (they follow their parent)\n", KN.size(), SN.size(), nMiss);
            }
            const auto KB = Skin.getBones();
            Out.resize(KB.size()); m_Delta.resize(KB.size());
            for (std::size_t s = 0; s < KB.size(); ++s)
            {
                const int a = m_Map[s], p = KB[s].m_iParent;
                // (an unmatched top bone moves with the source's root: left at rest it would pull its vertices back to the origin as the character walks off)
                m_Delta[s] = (a >= 0 && std::size_t(a) < SrcWorlds.size()) ? SrcWorlds[std::size_t(a)] * m_SrcInvRest[std::size_t(a)] : p >= 0 ? m_Delta[std::size_t(p)]
                           : !SrcWorlds.empty() ? SrcWorlds[0] * m_SrcInvRest[0] : xmath::fmat4::fromIdentity();
                Out[s] = m_Delta[s] * m_SkinRest[s];
            }
            static const bool bDiag = [] { const char* e = std::getenv("XLION_SKIN_UNDER_DIAG"); return e && *e == '1'; }();
            if (bDiag)
            {   // (narrowing: the bones stretched most from their parent, posed vs rest)
                std::vector<std::pair<float, int>> St;
                for (std::size_t s = 0; s < KB.size(); ++s) if (const int p = KB[s].m_iParent; p >= 0)
                    St.push_back({ std::abs((Out[s].ExtractPosition() - Out[std::size_t(p)].ExtractPosition()).Length() - (m_SkinRest[s].ExtractPosition() - m_SkinRest[std::size_t(p)].ExtractPosition()).Length()), int(s) });
                std::sort(St.rbegin(), St.rend());
                for (std::size_t k = 0; k < St.size() && k < 4; ++k) std::printf("[SkinUnder] stretch %.3f at skin bone %d (source %d, parent %d)\n", St[k].first, St[k].second, m_Map[std::size_t(St[k].second)], KB[std::size_t(St[k].second)].m_iParent);
            }
            static const bool bRest = [] { const char* e = std::getenv("XLION_SKIN_UNDER_REST"); return e && *e == '1'; }();     // (narrowing: the skin at its own rest)
            if (bRest) Out = m_SkinRest;
        }
        const xskeleton::skeleton*  m_pMapSrc = nullptr; const xskeleton::skeleton* m_pMapSkin = nullptr;
        std::vector<int>            m_Map;
        std::vector<xmath::fmat4>   m_SrcRest, m_SrcInvRest, m_SkinRest, m_Delta;

        // the Skin toggle every character view shows in its camera toolbar
        static void Button(bool& bShow, bool bHorizontal, float ButtonHeight, const char* pWhat = "The skin, lit, under the bones") noexcept
        {
            if (bHorizontal) ImGui::SameLine();
            const bool bOn = bShow;
            if (bOn) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Header]);
            if (ImGui::Button(bHorizontal ? "Skin###ShowSkin" : "S###ShowSkin", bHorizontal ? ImVec2(52.0f, ButtonHeight) : ImVec2(32.0f, ButtonHeight))) bShow = !bShow;
            if (bOn) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) xeditor::hint::Text("%s (%s)", pWhat, bOn ? "on" : "off");
        }
    };
}
#endif
