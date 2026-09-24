#ifndef XGEOM_SKIN_EDITOR_H
#define XGEOM_SKIN_EDITOR_H
#pragma once

// The Skin Geom editor: opens a skinned geometry resource from the asset browser in its own window. The descriptor is edited in an inspector
// and through the node commands (merge groups, deleted nodes), all undoable; the compiled geometry is previewed in 3D deformed by the pose of
// its skeleton, or by an animation package playing on it. Hosts include this header and open editors through xeditor::open_resource_editors.
#include "source/Tools/Editor/xeditor_descriptor_editor.h"
#include "dependencies/xresource_pipeline_v2/source/editor/E10_InspectorPickers.h"
#include "dependencies/xresource_pipeline_v2/source/editor/E10_Resources.h"
#include "plugins/xgeom_skin.plugin/source/Editor/xgeom_skin_editor_preview.h"
#include "plugins/xgeom_skin.plugin/source/Editor/xgeom_skin_thumbnail.h"
#include "plugins/xgeom_skin.plugin/source/xgeom_skin_xgpu_rsc_loader.cpp"      // the resource loader: compiled once, in the host's translation unit
#include "source/tools/xgpu_imgui_timeline.h"

#include <charconv>
#include <functional>

namespace xgeom_skin_editor
{
    //--------------------------------------------------------------------------------------------
    // Node commands. A node is named by its path from the root of the imported scene ("Root/Body/Wheel", base64 on the command line).
    // Each one snapshots the descriptor first, so undo puts back everything it touched (groups, ungrouped meshes, deleted list, material counts).
    //--------------------------------------------------------------------------------------------
    struct node_cmd : xundo::command_base
    {
        using apply_fn = std::string(*)(xgeom_skin::descriptor&, const xgeom_skin::details&, const std::string& Path, int Group);

        xeditor::descriptor_document&   m_Doc;
        xgeom_skin::details&          m_Details;
        const char*                     m_pHelp;
        apply_fn                        m_Apply;
        bool                            m_bGroupArg;

        node_cmd(xundo::system& System, xeditor::descriptor_document& Doc, xgeom_skin::details& Details, const char* pName, const char* pHelp, apply_fn Apply, bool bGroupArg) noexcept
            : command_base(System, pName, nullptr), m_Doc(Doc), m_Details(Details), m_pHelp(pHelp), m_Apply(Apply), m_bGroupArg(bGroupArg) { RegisterArguments(); }

        const char* getCommandHelp() const noexcept override { return m_pHelp; }
        void RegisterArguments() noexcept override
        {
            m_hNode = m_Parser.addOption("Node", "Node path from the scene root, base64", true, 1);
            if (m_bGroupArg) m_hGroup = m_Parser.addOption("Group", "Index of the merge group", true, 1);
        }

        std::string Redo() noexcept override
        {
            std::string Node, Group;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hNode, Node) || (m_bGroupArg && !xeditor::cmd_util::GetArg(m_Parser, m_hGroup, Group)))
                return std::format("{}: bad arguments", m_pCommandName);
            if (!m_Doc.m_pDescriptor) return std::format("{}: nothing loaded", m_pCommandName);

            int iGroup = 0;
            if (m_bGroupArg) std::from_chars(Group.data(), Group.data() + Group.size(), iGroup);

            const auto Path = xeditor::Base64Decode(Node);
            if (!m_Details.findNode(xgeom_skin::SplitNodePath(Path)).first) return std::format("{}: no node '{}' (compile the geometry first)", m_pCommandName, Path);

            auto Err = m_Apply(static_cast<xgeom_skin::descriptor&>(*m_Doc.m_pDescriptor), m_Details, Path, iGroup);
            if (!Err.empty()) return std::format("{}: {}", m_pCommandName, Err);
            m_Doc.m_bDirty = true;
            return {};
        }

        void BackupCurrenState(xundo::undo_file& File) noexcept override { xeditor::WriteString(File, m_Doc.Snapshot()); }
        void Undo(xundo::undo_file& File) noexcept override               { m_Doc.Restore(xeditor::ReadString(File)); }

        xcmdline::parser::handle m_hNode, m_hGroup;
    };

    inline std::string AddToNewGroup(xgeom_skin::descriptor& D, const xgeom_skin::details&, const std::string& Path, int)
    {
        for (int i = 0; i < 100; ++i)
        {
            auto Name = std::format("Group #{}", i);
            if (std::ranges::any_of(D.m_MergeGroupList, [&](auto& G) { return G.m_Name == Name; })) continue;
            auto& Group = D.m_MergeGroupList.emplace_back();
            Group.m_Name = std::move(Name);
            D.AddNodeInGroupList(Group, Path);
            return {};
        }
        return "too many merge groups";
    }

    inline std::string AddToGroup(xgeom_skin::descriptor& D, const xgeom_skin::details&, const std::string& Path, int Group)
    {
        if (Group < 0 || Group >= static_cast<int>(D.m_MergeGroupList.size())) return std::format("no merge group {}", Group);
        D.AddNodeInGroupList(D.m_MergeGroupList[Group], Path);
        return {};
    }

    inline std::string RemoveFromGroup(xgeom_skin::descriptor& D, const xgeom_skin::details& Details, const std::string& Path, int)
    {
        auto Pair = D.findMergeGroupFromNode(Path);
        if (!Pair.first) return "the node is not in a merge group";
        D.RemoveNodeFromGroup(*Pair.first, Pair.second, Details);
        return {};
    }

    inline std::string DeleteNode(xgeom_skin::descriptor& D, const xgeom_skin::details& Details, const std::string& Path, int)
    {
        if (D.isNodeInDeleteList(Path)) return "the node is already deleted";
        D.AddNodeInDeleteList(Path, Details);
        return {};
    }

    inline std::string UndeleteNode(xgeom_skin::descriptor& D, const xgeom_skin::details& Details, const std::string& Path, int)
    {
        if (!std::ranges::any_of(D.m_DeleteEntryList, [&](auto& E) { return E.m_MeshName.empty() && E.m_NodePath == Path; })) return "the node itself is not deleted";
        D.RemoveNodeFromDeleteList(Path, Details);
        return {};
    }

    // The nodes with the state that decides what the compiler does with them, one per line: path, group, deleted.
    struct list_nodes_cmd : xundo::query_command_base
    {
        xeditor::descriptor_document&   m_Doc;
        xgeom_skin::details&          m_Details;
        list_nodes_cmd(xundo::system& System, xeditor::descriptor_document& Doc, xgeom_skin::details& Details) noexcept : query_command_base(System, "ListNodes", nullptr), m_Doc(Doc), m_Details(Details) {}
        const char* getCommandHelp() const noexcept override { return "Lists the scene nodes of the compiled geometry with their merge group and deleted state. Usage: ListNodes"; }
        void RegisterArguments() noexcept override {}
        std::string Query() noexcept override
        {
            if (!m_Doc.m_pDescriptor) return "ListNodes: nothing loaded";
            if (m_Details.m_RootNode.m_Name.empty()) return "ListNodes: no node information yet (compile the geometry first)";
            auto& D = static_cast<xgeom_skin::descriptor&>(*m_Doc.m_pDescriptor);

            std::string Out;
            std::function<void(const xgeom_skin::details::node&, const std::string&)> Walk = [&](const xgeom_skin::details::node& Node, const std::string& Path)
            {
                Out += Path;
                if (auto* pGroup = D.findMergeGroupFromNode(Path).first) Out += std::format("  [group {}]", pGroup->m_Name);
                if (D.isNodeInDeleteList(Path)) Out += "  [deleted]";
                for (auto i : Node.m_MeshList) Out += std::format("  mesh:{}", m_Details.m_MeshList[i].m_Name);
                Out += '\n';
                for (auto& Child : Node.m_Children) Walk(Child, Path + "/" + Child.m_Name);
            };
            Walk(m_Details.m_RootNode, m_Details.m_RootNode.m_Name);
            return Out;
        }
    };


    //--------------------------------------------------------------------------------------------
    // Playback commands: which clip of the preview animation plays, and where. Playing is view state, not part of the resource, so these are
    // queries: not undoable, and they never dirty the descriptor.
    //--------------------------------------------------------------------------------------------
    struct session;

    struct playback_cmd : xundo::query_command_base
    {
        enum class kind { select_clip, play, pause, seek, set_speed, info };

        session&    m_Session;
        kind        m_Kind;
        const char* m_pHelp;

        playback_cmd(xundo::system& System, session& Session, kind Kind, const char* pName, const char* pHelp) noexcept
            : query_command_base(System, pName, nullptr), m_Session(Session), m_Kind(Kind), m_pHelp(pHelp) { RegisterArguments(); }

        const char* getCommandHelp() const noexcept override { return m_pHelp; }
        void RegisterArguments() noexcept override
        {
            switch (m_Kind)
            {
            case kind::select_clip: m_hA = m_Parser.addOption("Clip",  "Clip index in the preview animation", true, 1); break;
            case kind::seek:        m_hA = m_Parser.addOption("Time",  "Seconds from the clip start",         true, 1); break;
            case kind::set_speed:   m_hA = m_Parser.addOption("Index", "Playback speed step (0.25x .. 3x)",   true, 1); break;
            default: break;
            }
        }
        std::string Query() noexcept override;

        xcmdline::parser::handle m_hA;
    };

    //--------------------------------------------------------------------------------------------
    // The editor
    //--------------------------------------------------------------------------------------------
    struct session : xeditor::descriptor_editor
    {
        using desc = xgeom_skin::descriptor;

        xgeom_skin::details                                 m_Details;                  // the scene nodes, from the compiler's log (empty until the first compile)
        node_cmd                                            m_AddToNewGroup, m_AddToGroup, m_RemoveFromGroup, m_DeleteNode, m_UndeleteNode;
        list_nodes_cmd                                      m_ListNodes;
        xeditor::set_preview_cmd<preview::render_settings>  m_SetPreview;
        xeditor::list_preview_cmd<preview::render_settings> m_ListPreview;
        playback_cmd                                        m_SelectClip, m_Play, m_Pause, m_Seek, m_SetSpeed, m_Info;

        preview::runtime                                    m_Preview;
        xeditor::camera_cmds                                m_CameraCmds;
        preview::render_settings                            m_Settings;
        xeditor::inspector_panel                            m_SettingsInspector{ "Rendering Settings" };

        xrsc::geom_skin                                     m_GeomRef;
        xrsc::skeleton                                      m_SkeletonRef;
        bool                                                m_bHasGeom = false;         // the compiled geometry and its skeleton are loaded and drawable
        std::string                                         m_ErrorMessage;             // why nothing is drawn, when so
        std::vector<xmath::fmat4>                           m_PoseWorlds;
        std::uint64_t                                       m_LoadedSkeleton = 0;       // the skeleton the descriptor named when the geometry was loaded

        // The preview animation: tracked apart from the setting, which is never resolved (resolving turns a reference into a pointer), so a change can be seen
        xrsc::anim_package                                  m_AnimRef;
        std::uint64_t                                       m_LastAnimInstance = 0;
        int                                                 m_iSelectedClip = -1;
        float                                               m_TimeSeconds = 0.0f;
        int                                                 m_LoopsElapsed = 0;
        bool                                                m_bPlaying = false;
        int                                                 m_iSpeedIndex = xgpu::tools::editors::g_DefaultSpeedIndex;
        xgpu::tools::imgui::timeline::state                 m_Timeline;

        session(xresource::full_guid Guid, e10::library::guid LibraryGuid, xgpu::device* pDevice) noexcept
            : descriptor_editor("SkinGeom", Guid, LibraryGuid, pDevice)
            , m_AddToNewGroup   (m_Undo, m_Document, m_Details, "AddNodeToNewGroup",  "Puts a node in a new merge group (undoable). Usage: AddNodeToNewGroup -Node base64",            &AddToNewGroup,   false)
            , m_AddToGroup      (m_Undo, m_Document, m_Details, "AddNodeToGroup",     "Puts a node in a merge group (undoable). Usage: AddNodeToGroup -Node base64 -Group index",      &AddToGroup,      true)
            , m_RemoveFromGroup (m_Undo, m_Document, m_Details, "RemoveNodeFromGroup","Takes a node out of its merge group (undoable). Usage: RemoveNodeFromGroup -Node base64",         &RemoveFromGroup, false)
            , m_DeleteNode      (m_Undo, m_Document, m_Details, "DeleteNode",         "Leaves a node (and its children) out of the compiled geometry (undoable). Usage: DeleteNode -Node base64", &DeleteNode, false)
            , m_UndeleteNode    (m_Undo, m_Document, m_Details, "UndeleteNode",       "Puts a deleted node back (undoable). Usage: UndeleteNode -Node base64",                         &UndeleteNode,    false)
            , m_ListNodes(m_Undo, m_Document, m_Details)
            , m_SetPreview(m_Undo, m_Settings), m_ListPreview(m_Undo, m_Settings)
            , m_CameraCmds(m_Undo, m_Preview.Camera())
            , m_SelectClip(m_Undo, *this, playback_cmd::kind::select_clip, "SelectClip",   "Selects a clip of the preview animation. Usage: SelectClip -Clip index")
            , m_Play      (m_Undo, *this, playback_cmd::kind::play,        "Play",         "Plays the selected clip. Usage: Play")
            , m_Pause     (m_Undo, *this, playback_cmd::kind::pause,       "Pause",        "Pauses the playback. Usage: Pause")
            , m_Seek      (m_Undo, *this, playback_cmd::kind::seek,        "Seek",         "Moves the playback to a time in seconds. Usage: Seek -Time seconds")
            , m_SetSpeed  (m_Undo, *this, playback_cmd::kind::set_speed,   "SetSpeed",     "Sets the playback speed step (0 = 0.25x, 3 = 1x, 7 = 3x). Usage: SetSpeed -Index step")
            , m_Info      (m_Undo, *this, playback_cmd::kind::info,        "PlaybackInfo", "The preview animation, the clip, the time and whether it plays. Usage: PlaybackInfo")
        {
            // Reading the descriptor also cross-checks the skeleton's compiled bone list: not having it yet does not make the geometry unusable
            m_Document.m_TolerateReadError = [](const xresource_pipeline::descriptor::base& D) { return !static_cast<const desc&>(D).m_SkeletonRef.empty(); };
            m_Document.Load();
            BindDescriptorInspector();
            e10::WireResourcePickerCallbacks(m_DescriptorInspector.m_Inspector);
            e10::WireResourcePickerCallbacks(m_SettingsInspector.m_Inspector);
            RegisterMaterialRowLabels(m_DescriptorInspector.m_Inspector);
            m_SettingsInspector.BindObject(*xproperty::getObjectByType<preview::render_settings>(), &m_Settings);

            AddPanel("Rendering Settings", dock::left,   [this] { m_SettingsInspector.Show(); });
            AddPanel("Skin Viewport",      dock::center, [this] { RenderViewport(); }, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            AddPanel("Playback",           dock::bottom, [this] { RenderPlayback(); });
            AddPanel("Description",        dock::right,  [this] { RenderHierarchy(); m_DescriptorInspector.Show(); });

            if (pDevice && m_Preview.Init(*pDevice)) ReloadResource();
        }

        ~session() noexcept override
        {
            xresource::g_Mgr.ReleaseRef(m_GeomRef);
            xresource::g_Mgr.ReleaseRef(m_SkeletonRef);
            xresource::g_Mgr.ReleaseRef(m_AnimRef);
            m_Preview.Release();
        }

        desc* Desc() noexcept { return m_Document.isLoaded() ? static_cast<desc*>(m_Document.m_pDescriptor.get()) : nullptr; }

        void OnCompileStarted() noexcept override { LetGo(); }
        void OnCompiled()       noexcept override { ReloadResource(); }
        // An undo puts the descriptor back; the compiled geometry only needs loading again when the skeleton it is skinned to changed
        void OnDescriptorReplaced() noexcept override
        {
            if (auto* pDesc = Desc(); pDesc && pDesc->m_SkeletonRef.m_Instance.m_Value != m_LoadedSkeleton) ReloadResource();
        }

        // Each material slot of the descriptor is labelled with the material's name, and dimmed when the current nodes no longer use it.
        static void RegisterMaterialRowLabels(xproperty::inspector& Inspector) noexcept
        {
            Inspector.m_OnResourceLeftSize.m_Delegates.clear();
            Inspector.m_OnResourceLeftSize.Register<[](xproperty::inspector&, const xproperty::type::object&, void* pInstance, std::string_view Path, const xproperty::any&, ImGuiTreeNodeFlags Flags, const char* pName, bool& Open)
            {
                std::string NewName;
                bool bDisable     = false;
                bool bIsMaterialSlot = false;

                // Only the material slots are relabelled: the delegate is called for every resource row of the inspector. (Matched by
                // path: the property table object differs between translation units, so its address says nothing.)
                if (Path.find("/MaterialInstance[") != std::string_view::npos)
                {
                    const auto Bracket = Path.rfind('[');
                    const auto Colon   = Bracket == std::string_view::npos ? std::string_view::npos : Path.find(':', Bracket);
                    const auto Close   = Bracket == std::string_view::npos ? std::string_view::npos : Path.find(']', Bracket);
                    auto pDesc = static_cast<xgeom_skin::descriptor*>(pInstance);
                    if (Colon != std::string_view::npos && Close != std::string_view::npos && Colon < Close && !pDesc->m_MaterialDetailsList.empty())
                    {
                        const auto Text = Path.substr(Colon + 1, Close - Colon - 1);
                        int Index = 0;
                        if (std::from_chars(Text.data(), Text.data() + Text.size(), Index).ec == std::errc() && Index >= 0 && Index < static_cast<int>(pDesc->m_MaterialDetailsList.size()))
                        {
                            NewName       = std::format("{} {}", pName, pDesc->m_MaterialDetailsList[Index].m_Name);
                            pName         = NewName.c_str();
                            bDisable      = pDesc->m_MaterialDetailsList[Index].m_RefCount <= 0;
                            bIsMaterialSlot = true;
                        }
                    }
                }

                if (bDisable) ImGui::BeginDisabled(true);
                // The "  " (two leading spaces) gives an ordinary resource-ref row - no arrow, no icon
                // cluster of its own - a little breathing room against the Framed box's own left edge.
                // A material-slot row is a per-element row of the array-controls cluster (drag/insert/
                // delete icons already sit immediately to its left), so that same padding reads as an
                // unwanted gap between the trashcan icon and the "[i]" index there - dropped for that
                // case specifically, kept for every other resource-ref row using this same delegate.
                if (!Path.empty()) Open = ImGui::TreeNodeEx(reinterpret_cast<const void*>(std::hash<std::string_view>{}(Path)), ImGuiTreeNodeFlags_Framed | Flags, bIsMaterialSlot ? "%s" : "  %s", pName);
                else               Open = ImGui::TreeNodeEx(pName, Flags);
                if (bDisable) ImGui::EndDisabled();
            }>();
        }


        // The compiled geometry and the skeleton it is skinned to are let go when a compile starts: the files are about to be replaced
        void LetGo() noexcept
        {
            m_Preview.ReleaseMaterials();
            xresource::g_Mgr.ReleaseRef(m_GeomRef);
            xresource::g_Mgr.ReleaseRef(m_SkeletonRef);
            m_GeomRef.clear();
            m_SkeletonRef.clear();
            m_bHasGeom = false;
        }

        // Loads the compiled geometry (after a compile, or when the editor opens on an already compiled one), the skeleton it is skinned to, the
        // preview's materials and the scene nodes the compiler left in its log.
        void ReloadResource() noexcept
        {
            LetGo();
            m_ErrorMessage.clear();
            auto* pDesc = Desc();
            if (!m_Preview.m_bReady) { m_ErrorMessage = "The preview needs a GPU device (open from E29)."; return; }
            if (!pDesc) { m_ErrorMessage = "The descriptor could not be read."; return; }
            LoadDetails();
            if (!std::filesystem::exists(m_Document.m_ResourcePath)) { m_ErrorMessage = "No compiled resource yet: compile the geometry."; return; }

            m_GeomRef.m_Instance = m_Document.m_Guid.m_Instance;
            auto* pGeom = xresource::g_Mgr.getResource(m_GeomRef);
            if (!pGeom) { m_GeomRef.clear(); m_ErrorMessage = "The compiled geometry could not be loaded."; return; }
            if (pDesc->m_SkeletonRef.empty()) { m_ErrorMessage = "This geometry's descriptor has no Skeleton reference."; return; }

            m_SkeletonRef.m_Instance = pDesc->m_SkeletonRef.m_Instance;
            m_LoadedSkeleton = pDesc->m_SkeletonRef.m_Instance.m_Value;
            auto* pSkeleton = xresource::g_Mgr.getResource(m_SkeletonRef);
            if (!pSkeleton) { m_SkeletonRef.clear(); m_ErrorMessage = "The referenced skeleton could not be loaded."; return; }

            xgpu::tools::editors::ComputeRestBoneWorlds(*pSkeleton, m_PoseWorlds);
            m_Preview.m_Center   = pGeom->m_BBox.getCenter();
            m_Preview.m_Radius   = std::max(0.01f, pGeom->m_BBox.getRadius());
            m_Preview.m_bReframe = true;
            m_Preview.RebuildMaterials(*pGeom);
            m_bHasGeom = true;
        }

        // The compiler's list of nodes and meshes. The descriptor is brought in line with it: new meshes appear, vanished ones are dropped.
        void LoadDetails() noexcept
        {
            xtextfile::stream File;
            if (auto Err = File.Open(true, m_Document.m_LogPath + L"\\Details.txt", {}); Err) return;

            m_Details = {};
            xproperty::settings::context Context;
            if (auto Err = xproperty::sprop::serializer::Stream(File, m_Details, Context); Err) { m_Details = {}; return; }

            const auto Before = m_Document.Snapshot();
            for (auto& Message : static_cast<xgeom_skin::descriptor&>(*m_Document.m_pDescriptor).MergeWithDetails(m_Details))
                std::printf("%s\n", Message.c_str());
            if (m_Document.Snapshot() != Before) m_Document.m_bDirty = true;
        }


        //----------------------------------------------------------------------------------------
        // Playback
        //----------------------------------------------------------------------------------------

        xanim_package::anim_package* Anim() noexcept { return m_AnimRef.empty() ? nullptr : xresource::g_Mgr.getResource(m_AnimRef); }

        // The animation the preview plays on this skeleton: it has to agree with it on the bones
        xanim_package::anim_package* UsableAnim(const xskeleton::skeleton& Skeleton) noexcept
        {
            auto* pAnim = Anim();
            return pAnim && !pAnim->getClips().empty() && pAnim->m_nBones == Skeleton.getBones().size() ? pAnim : nullptr;
        }

        static float ClipLength(const xanim_package::clip& Clip) noexcept
        {
            return (Clip.m_FPS > 0 && Clip.m_nFrames > 0) ? float(Clip.m_nFrames) / float(Clip.m_FPS) : 0.0f;
        }

        void SelectClip(int iClip) noexcept
        {
            m_iSelectedClip = iClip;
            m_TimeSeconds   = 0.0f;
            m_LoopsElapsed  = 0;
            m_Timeline      = {};
        }

        // Once a frame, whichever tab is showing: a changed preview animation is picked up, and the time advances while playing
        void Render() noexcept override
        {
            if (m_Settings.m_PreviewAnimRef.m_Instance.m_Value != m_LastAnimInstance)
            {
                xresource::g_Mgr.ReleaseRef(m_AnimRef);
                m_AnimRef           = m_Settings.m_PreviewAnimRef;
                m_LastAnimInstance  = m_Settings.m_PreviewAnimRef.m_Instance.m_Value;
                SelectClip(-1);
                m_bPlaying          = !m_AnimRef.empty();
            }

            if (m_bPlaying && m_Settings.m_PoseType == preview::pose_type::ANIMATION_POSE)
                if (auto* pAnim = Anim(); pAnim && m_iSelectedClip >= 0 && m_iSelectedClip < int(pAnim->getClips().size()))
                {
                    auto& Clip = pAnim->getClips()[m_iSelectedClip];
                    if (ClipLength(Clip) > 0.0f)
                        xgpu::tools::editors::AdvancePlayback(m_TimeSeconds, m_LoopsElapsed, m_bPlaying, ClipLength(Clip), Clip.m_bLoop, ImGui::GetIO().DeltaTime, xgpu::tools::editors::g_PlaybackSpeeds[m_iSpeedIndex]);
                }
            descriptor_editor::Render();
        }

        // The pose of the bones this frame: the animation at the current time (with its root motion) or the rest pose
        void EvaluatePose(const xskeleton::skeleton& Skeleton) noexcept
        {
            auto* pAnim = UsableAnim(Skeleton);
            if (m_Settings.m_PoseType == preview::pose_type::ANIMATION_POSE && pAnim)
            {
                if (m_iSelectedClip < 0 || m_iSelectedClip >= int(pAnim->getClips().size())) SelectClip(0);
                auto& Clip = pAnim->getClips()[m_iSelectedClip];
                xgpu::tools::editors::ComputeAnimatedBoneWorlds(Skeleton, *pAnim, m_iSelectedClip, m_TimeSeconds, m_PoseWorlds);
                if (Clip.m_RootMotionMode != xanim_package::root_motion_mode::NONE)
                    xgpu::tools::editors::ApplyWorldOffset(m_PoseWorlds, xgpu::tools::editors::ComputeRootMotionOffset(Clip, pAnim->getClipRootMotion(m_iSelectedClip), m_TimeSeconds, m_LoopsElapsed));
            }
            else xgpu::tools::editors::ComputeRestBoneWorlds(Skeleton, m_PoseWorlds);
        }

        void RenderViewport() noexcept
        {
            auto* pHost   = xeditor::host::current();
            auto* pWindow = pHost ? pHost->find<xgpu::window>() : nullptr;
            const ImVec2 Avail = ImGui::GetContentRegionAvail();
            const ImVec2 Min   = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(Min, ImVec2(Min.x + Avail.x, Min.y + Avail.y), IM_COL32(115, 115, 115, 255));
            ImGui::InvisibleButton("##SkinViewport", Avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
            m_Preview.HandleInput();

            if (!m_Preview.m_bReady || !pWindow) { ImGui::SetCursorScreenPos(Min); ImGui::TextDisabled("The 3D view needs a GPU device (open from E29)."); return; }
            auto* pGeom     = m_bHasGeom ? xresource::g_Mgr.getResource(m_GeomRef) : nullptr;
            auto* pSkeleton = m_bHasGeom ? xresource::g_Mgr.getResource(m_SkeletonRef) : nullptr;
            if (!pGeom || !pSkeleton) { ImGui::SetCursorScreenPos(Min); ImGui::TextWrapped("%s", m_ErrorMessage.empty() ? "Nothing to show." : m_ErrorMessage.c_str()); return; }

            m_Preview.UpdateView(Min, Avail.x, Avail.y);
            EvaluatePose(*pSkeleton);
            m_Preview.UploadBones(*pSkeleton, m_PoseWorlds, m_Settings.m_PoseType);
            m_Preview.RenderShadow(*pWindow, *pGeom, m_Settings);

            xgpu::tools::imgui::AddCustomRenderCallback([this](xgpu::cmd_buffer& CmdBuffer, const ImVec2&, const ImVec2&)
            {
                if (!m_bOpen || !m_bHasGeom) return;
                if (auto* p = xresource::g_Mgr.getResource(m_GeomRef)) m_Preview.DrawGeom(CmdBuffer, *p, m_Settings);
            });
        }

        void RenderPlayback() noexcept
        {
            auto* pAnim = Anim();
            if (!pAnim || pAnim->getClips().empty()) { ImGui::TextDisabled("Pick a preview animation in the Rendering Settings to play it on the geometry."); return; }
            namespace ed = xgpu::tools::editors;
            if (m_iSelectedClip < 0 || m_iSelectedClip >= int(pAnim->getClips().size())) SelectClip(0);
            auto& Clip = pAnim->getClips()[m_iSelectedClip];
            const float Length = ClipLength(Clip);

            if (ImGui::BeginCombo("Clip", std::format("Clip {}", m_iSelectedClip).c_str()))
            {
                for (int i = 0; i < int(pAnim->getClips().size()); ++i)
                    if (ImGui::Selectable(std::format("Clip {}", i).c_str(), i == m_iSelectedClip)) SelectClip(i);
                ImGui::EndCombo();
            }

            if (ImGui::Button(m_bPlaying ? ed::g_PauseIcon : ed::g_PlayIcon)) m_bPlaying = !m_bPlaying;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(m_bPlaying ? "Pause" : "Play");
            ImGui::SameLine();
            if (ImGui::Button(ed::g_GoToStartIcon)) { m_TimeSeconds = 0.0f; m_LoopsElapsed = 0; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to start");
            ImGui::SameLine();
            if (ImGui::Button(ed::g_GoToEndIcon)) m_TimeSeconds = Length;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to end");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0f);
            ImGui::SliderInt("##speed", &m_iSpeedIndex, 0, ed::g_NumPlaybackSpeeds - 1, ed::g_PlaybackSpeedLabels[m_iSpeedIndex]);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Playback speed");

            const float Footer = ImGui::GetTextLineHeightWithSpacing();
            if (xgpu::tools::imgui::timeline::Draw(m_Timeline, m_TimeSeconds, Length, static_cast<float>(Clip.m_FPS), {}, "playback_timeline", "Clip", std::max(ImGui::GetContentRegionAvail().y - Footer, 0.0f)))
                m_LoopsElapsed = 0;

            ImGui::Text("Zoom: %.0f%%    FPS: %d    Frames: %d    Loop: %s", xgpu::tools::imgui::timeline::GetZoomPercent(m_Timeline, Length), Clip.m_FPS, Clip.m_nFrames, Clip.m_bLoop ? "Yes" : "No");
        }

        // The scene nodes. Right click: merge groups and deleting. Every action is a command, run once the tree is drawn.
        void RenderHierarchy() noexcept
        {
            auto& Root = m_Details.m_RootNode;
            if (!m_Document.m_pDescriptor || (Root.m_Children.empty() && Root.m_MeshList.empty())) return;
            xeditor::PushLevelEditorInspectorHeaderColors();
            const bool bShow = ImGui::CollapsingHeader("Scene Hierarchy", ImGuiTreeNodeFlags_DefaultOpen);
            xeditor::PopLevelEditorInspectorHeaderColors();
            if (!bShow) return;

            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 12));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(8, 7));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(8, 2));
            ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 12.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 8));

            constexpr ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth;
            auto&  Desc         = static_cast<xgeom_skin::descriptor&>(*m_Document.m_pDescriptor);
            const auto TextColor    = ImGui::GetStyle().Colors[ImGuiCol_Text];
            const auto GroupColor   = ImVec4(0.5f, 1.0f, 0.5f, 1.0f);
            const auto DeletedColor = ImVec4(0.8f, 0.3f, 0.3f, 1.0f);
            std::string Pending;                                    // the command a menu item asked for

            // A node with nothing to show below it is not drawn
            std::function<bool(const xgeom_skin::details::node&)> WorthRendering = [&](const xgeom_skin::details::node& N)
            {
                if (!N.m_MeshList.empty()) return true;
                return std::ranges::any_of(N.m_Children, [&](auto& C) { return WorthRendering(C); });
            };

            auto Command = [&](const char* pName, const std::string& Path) { return std::format("{} -Node {}", pName, xeditor::Base64Encode(Path)); };

            std::string Path = Root.m_Name;
            std::function<void(const xgeom_skin::details::node&, bool, bool)> DisplayNode = [&](const xgeom_skin::details::node& N, bool bIncluded, bool bDeletedParent)
            {
                if (!WorthRendering(N)) return;

                const size_t PrevLength = Path.size();
                Path += "/" + N.m_Name;

                const bool  bInDeleteList = Desc.isNodeInDeleteList(Path);
                const bool  bDeleted      = bDeletedParent || bInDeleteList;
                const auto  Pair          = Desc.findMergeGroupFromNode(Path);
                const char* pGroupName    = Pair.first ? Pair.first->m_Name.c_str() : "";

                if (bDeleted) ImGui::PushStyleColor(ImGuiCol_Text, DeletedColor);
                const bool bOpen = bInDeleteList ? ImGui::TreeNodeEx(&N, Flags, "\xEE\x9D\x8D (%s) %s", pGroupName, N.m_Name.c_str())
                                 : Pair.first    ? ImGui::TreeNodeEx(&N, Flags, "\xEE\xAF\x92 (%s) %s", pGroupName, N.m_Name.c_str())
                                                 : ImGui::TreeNodeEx(&N, Flags, "%s", N.m_Name.c_str());
                if (bDeleted) ImGui::PopStyleColor();

                ImGui::PushID(&N);
                if (ImGui::BeginPopupContextItem("NodeContextMenu"))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, TextColor);
                    if (!bIncluded)
                    {
                        if (Pair.first)
                        {
                            if (ImGui::MenuItem("Remove from Group")) Pending = Command("RemoveNodeFromGroup", Path);
                        }
                        else
                        {
                            if (ImGui::MenuItem("Add to New Group")) Pending = Command("AddNodeToNewGroup", Path);
                            if (!Desc.m_MergeGroupList.empty() && ImGui::BeginMenu("Add to Merge Group"))
                            {
                                for (int i = 0; i < static_cast<int>(Desc.m_MergeGroupList.size()); ++i)
                                    if (ImGui::MenuItem(Desc.m_MergeGroupList[i].m_Name.c_str())) Pending = std::format("AddNodeToGroup -Node {} -Group {}", xeditor::Base64Encode(Path), i);
                                ImGui::EndMenu();
                            }
                        }
                    }
                    if (!bDeleted)          { if (ImGui::MenuItem("\xEE\x9D\x8D Delete Node"))   Pending = Command("DeleteNode", Path); }
                    else if (bInDeleteList) { if (ImGui::MenuItem("\xEE\x9D\x8D UnDelete Node")) Pending = Command("UndeleteNode", Path); }
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }
                ImGui::PopID();

                if (bOpen)
                {
                    if (bDeleted) ImGui::PushStyleColor(ImGuiCol_Text, DeletedColor);
                    else if (Pair.first) ImGui::PushStyleColor(ImGuiCol_Text, GroupColor);

                    for (int Index : N.m_MeshList)
                    {
                        const auto& Mesh = m_Details.m_MeshList[Index];
                        ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<intptr_t>(Index)), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen, "\xEE\xAF\x92 %s", Mesh.m_Name.c_str());
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::PushStyleColor(ImGuiCol_Text, TextColor);
                            ImGui::Text("nFaces    : %d\nnUVs      : %d\nnColors   : %d\nnMaterials: %d\n", Mesh.m_NumFaces, Mesh.m_NumUVs, Mesh.m_NumColors, static_cast<int>(Mesh.m_MaterialList.size()));
                            for (auto& Mat : Mesh.m_MaterialList)
                                ImGui::Text("%2d.%s\n", 1 + static_cast<int>(&Mat - Mesh.m_MaterialList.data()), m_Details.m_MaterialList[Mat].c_str());
                            ImGui::PopStyleColor();
                            ImGui::EndTooltip();
                        }
                    }

                    for (auto& Child : N.m_Children) DisplayNode(Child, Pair.first || bIncluded, bDeleted);
                    ImGui::TreePop();

                    if (bDeleted || Pair.first) ImGui::PopStyleColor();
                }

                Path.resize(PrevLength);
            };

            const bool bRootOpen = ImGui::TreeNodeEx(&Root, Flags, Desc.m_bMergeAllMeshes ? "\xEE\xAF\x92 Root" : "Root");
            if (bRootOpen)
            {
                if (Desc.m_bMergeAllMeshes) ImGui::PushStyleColor(ImGuiCol_Text, GroupColor);
                for (auto& Child : Root.m_Children) DisplayNode(Child, Desc.m_bMergeAllMeshes, false);
                ImGui::TreePop();
                if (Desc.m_bMergeAllMeshes) ImGui::PopStyleColor();
            }
            ImGui::PopStyleVar(4);

            if (!Pending.empty()) xeditor::Run(m_Undo, Pending);
        }

    };

    inline std::string playback_cmd::Query() noexcept
    {
        auto& S = m_Session;
        std::string A;
        auto* pAnim = S.Anim();
        switch (m_Kind)
        {
        case kind::select_clip:
        {
            int iClip = -1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, A) || std::from_chars(A.data(), A.data() + A.size(), iClip).ec != std::errc()) return "SelectClip: bad arguments";
            if (!pAnim || iClip < 0 || iClip >= int(pAnim->getClips().size())) return "SelectClip: no such clip (set the PreviewAnimRef of the preview settings first)";
            S.SelectClip(iClip);
            return std::format("SelectClip: {}", iClip);
        }
        case kind::play:
            if (!pAnim) return "Play: no preview animation (set PreviewAnimRef)";
            S.m_bPlaying = true;
            return "Play: playing";
        case kind::pause: S.m_bPlaying = false; return "Pause: paused";
        case kind::seek:
        {
            float Time = 0;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, A) || std::from_chars(A.data(), A.data() + A.size(), Time).ec != std::errc()) return "Seek: bad arguments";
            if (!pAnim || S.m_iSelectedClip < 0 || S.m_iSelectedClip >= int(pAnim->getClips().size())) return "Seek: no clip is selected";
            S.m_TimeSeconds = std::clamp(Time, 0.0f, session::ClipLength(pAnim->getClips()[S.m_iSelectedClip]));
            S.m_LoopsElapsed = 0;
            return std::format("Seek: {:.3f}s", S.m_TimeSeconds);
        }
        case kind::set_speed:
        {
            int Index = -1;
            if (!xeditor::cmd_util::GetArg(m_Parser, m_hA, A) || std::from_chars(A.data(), A.data() + A.size(), Index).ec != std::errc()) return "SetSpeed: bad arguments";
            if (Index < 0 || Index >= xgpu::tools::editors::g_NumPlaybackSpeeds) return std::format("SetSpeed: 0 to {}", xgpu::tools::editors::g_NumPlaybackSpeeds - 1);
            S.m_iSpeedIndex = Index;
            return std::format("SetSpeed: {}", xgpu::tools::editors::g_PlaybackSpeedLabels[Index]);
        }
        case kind::info:
        {
            std::string Text = std::format("geometry drawn: {}\n", S.m_bHasGeom ? "yes" : "no");
            if (!S.m_ErrorMessage.empty()) Text += "note: " + S.m_ErrorMessage + "\n";
            Text += std::format("preview animation: {} clips\n", pAnim ? int(pAnim->getClips().size()) : 0);
            if (pAnim && S.m_iSelectedClip >= 0 && S.m_iSelectedClip < int(pAnim->getClips().size()))
            {
                auto& Clip = pAnim->getClips()[S.m_iSelectedClip];
                Text += std::format("clip: {}\ntime: {:.3f} / {:.3f}s\nplaying: {}\nspeed: {}\n", S.m_iSelectedClip, S.m_TimeSeconds, session::ClipLength(Clip), S.m_bPlaying ? "yes" : "no", xgpu::tools::editors::g_PlaybackSpeedLabels[S.m_iSpeedIndex]);
            }
            return Text;
        }
        }
        return {};
    }

    inline const xeditor::auto_register_resource_editor g_Registration
    { xrsc::geom_skin_type_guid_v
    , [](xresource::full_guid Guid, e10::library::guid LibraryGuid, xgpu::device* pDevice) -> std::unique_ptr<xeditor::resource_editor>
      { return std::make_unique<session>(Guid, LibraryGuid, pDevice); }
    };

    inline const xeditor::auto_register_thumbnail_renderer g_ThumbReg{ xrsc::geom_skin_type_guid_v, []{ return std::make_unique<thumbnail_renderer>(); } };
}

#endif // XGEOM_SKIN_EDITOR_H
