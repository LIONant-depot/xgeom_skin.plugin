# The Skin Geom editor

Double-click a skinned geometry in the asset browser (or `OpenResourceEditor -Asset <guid>`): a 3D preview of the compiled geometry deformed by
its skeleton, the scene nodes the compiler found (merge groups and deleted nodes), and the descriptor. Everything the window changes goes
through commands on the editor's own undo system.

```
source/Editor/xgeom_skin_editor.h            the editor (session), the node and playback commands, its registration
source/Editor/xgeom_skin_editor_preview.h    the preview: shadow pass, skinned geometry, materials, view settings
```

The preview builds on the shared skeleton scene (`plugins/xskeleton.plugin/source/Editor/xskeleton_editor_scene.h`: camera, ground grid that
receives the shadow). A host includes `xgeom_skin_editor.h`, which registers the editor for the `GeomSkin` type and compiles the geometry's
resource loader into the host. The host provides an `xgpu::device` and the main `xgpu::window`: the light's view is drawn into a shadow map with a
render pass on the window before the frame's UI is rendered, then the geometry is drawn from the preview panel's render callback.

## Panels

| Panel | |
|---|---|
| Skin Viewport | the geometry; right drag turns, middle drag pans, the wheel zooms |
| Rendering Settings | the preview animation, the LOD shown, the maximum bone influences, and the pose (bind: no deformation, frozen: the skeleton's rest pose, animation) |
| Playback | for a preview animation: clip, play / pause, go to start / end, speed, timeline |
| Description | the scene hierarchy (right click a node) and the descriptor |

## Commands

Run as `<resource name>\<Command>`. Paths, values and node paths are base64.

| Command | |
|---|---|
| `ListProperties [-Filter text]`, `SetProperty -Path -Value [-Before]` | descriptor properties (undoable) |
| `ListOp -Path -Op Insert\|Delete\|Move -Index n [-ToIndex n]` | inserts, deletes or moves an element in the middle of a 1D array property (undoable); ordinal keys only |
| `ListNodes` | the scene nodes with their merge group and deleted state (after the first compile) |
| `AddNodeToNewGroup -Node`, `AddNodeToGroup -Node -Group index`, `RemoveNodeFromGroup -Node` | merge groups (undoable) |
| `DeleteNode -Node`, `UndeleteNode -Node` | leave a node and its children out of the compiled geometry (undoable) |
| `ListPreview`, `SetPreview -Path -Value` | the preview settings (view state); `RenderSettings/PreviewAnimRef` takes `<instance hex>, <type hex>` |
| `SelectClip -Clip i`, `Play`, `Pause`, `Seek -Time`, `SetSpeed -Index`, `PlaybackInfo` | the transport (view state) |
| `Save`, `Compile`, `Undo`, `Redo` | |
| `CompileStatus [-Lines n]` | how the last compile went: state, unsaved changes, validation errors, the end of the log |
| `SetCamera [-Yaw -Pitch -Distance -Target x,y,z]`, `GetCamera`, `FrameSubject` | the preview camera (degrees), read back, or refitted to the subject (view state, not undoable) |

Each node command snapshots the descriptor first, so undo restores everything it touched. A node is named by its path from the scene root,
e.g. `RootNode/Armature/Body`.
