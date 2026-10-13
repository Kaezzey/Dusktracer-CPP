# Project folders and scene files

Each `.dusk` file stores a complete scene. Its parent directory is the project
folder. Several scene files can share the same folder and asset library.

## Workflow

- **File > New Project** (`Ctrl+N`): choose a folder and scene filename. Use the
  Windows dialog's New Folder button to create a separate project. The scene
  starts empty; supported files already in that folder appear in the Content Drawer.
- **Open Scene** (`Ctrl+O`): restore a `.dusk` file.
- **Open Project Folder**: open its scene, or create `Scene.dusk` if there isn't
  one yet. If it contains several scenes, choose one through Open Scene.
- **Save** (`Ctrl+S`): save the current scene. An untitled scene asks for a location.
- **Save As** (`Ctrl+Shift+S`): save another scene, copying referenced assets into
  the destination project when necessary.
- **Refresh Project Assets**, also available in the Content Drawer: discover
  files added in Explorer and reload assets changed outside the editor.

Dragging a `.dusk` file or project folder into the editor opens it. You can also
launch `Dusktracer.exe "C:\Projects\Studio\Scene.dusk"` or pass a project folder.
Opening a project at launch skips loading the demo's models.

The title bar shows the scene name and an asterisk for unsaved changes. Opening
another project, creating a new one, or exiting offers Save, Discard, and Cancel.
Project switching and refreshing are unavailable during a render; Save remains
available. Render output stays associated with the project that started it.

## What is saved

The scene file contains objects and transforms, lights, materials and complete
shader graphs (including layout), texture bindings, mesh material defaults and
instance overrides, camera pose and lens, image dimensions, sampling and caustic
settings, denoiser and exposure settings, CPU/GPU device selection, and viewport
resolution mode. GPU resources and acceleration structures are rebuilt on open.
Version 2 stores GPU firefly suppression. Version 3 adds SSS profiles and RGB
diffusion distances. Version 1 and 2 scenes remain readable with neutral RGB
distances; version 1 enables firefly suppression by default. Version 4 adds the
SSS shading model and Unreal Subsurface/Subsurface Profile parameters; older
scenes load as legacy Burley diffusion. Version 5 adds Multiply by Base Color
for Unreal Subsurface; earlier scenes load with it off. Version 6 adds the
profile's World Unit Scale; earlier scenes use 0.1 cm, which keeps their mean
free paths in millimetres. Version 7 adds bucket rendering and hair settings.
Version 8 adds each material's Alpha Mode and the eye settings; earlier scenes
load with Blend, so alpha below the old cutoff now shows partly instead of
vanishing (choose Clip to restore the cutout). New saves use version 8.

```text
Studio/
  Scene.dusk
  Alternate lighting.dusk
  models/
  textures/
  Renders/
```

Supported assets are OBJ/FBX models and PNG/JPG/JPEG/TGA/BMP/HDR/PPM textures.
Discovery includes subdirectories, excluding hidden folders and `Renders`.
Discovered models enter the asset library; drag them into the viewport to create
instances. Files dropped directly into the editor also create model instances
or texture graph nodes according to the existing drop target.

Imports into a named project are copied into `models` or `textures`. Files already
inside the project stay in place. Imports into an untitled scene keep their source
references until Save packages them. Content hashes distinguish same-named files
and reuse identical imports. OBJ imports include MTL sidecars and their referenced
textures. FBX shading uses the materials and texture bindings authored in Dusk.

Copy the **whole project folder** to move it to another machine. Asset paths in
the scene file are relative. Moving only the `.dusk` file leaves its assets behind.
Missing local assets preserve their bindings and produce an open warning, so they
can be restored at the recorded location. A missing external asset prevents Save As
from claiming a complete package. The prior scene file remains intact on failure.

The existing demo and legacy material/asset manifests can still be read at normal
startup. Save As converts that setup into a project. Project sessions do not write
the old global manifests.

## Implementation and checks

`project.cpp` owns the versioned text format, validation, atomic manifest
replacement, relative path resolution, asset packing and discovery. One field
visitor drives serialization and allocation-free dirty-state hashing. Hashing is
throttled to four checks per second while idle, and runs synchronously when a
save decision is needed. There is no disk I/O or shader compilation in that check.
Large OBJ geometry is streamed while sidecar references are rewritten.

`editor_projects.inl` prepares a new scene and its GL meshes before replacing the
current scene. It clears old selections, undo actions and preview lookups.
Workers discard queued previews without joining an active job, and generations
reject late results. The process working directory never changes during project
operations. Windows Common Item Dialogs provide file and folder selection.

Run `ctest --test-dir build -C Release --output-on-failure`. The
`project_persistence_and_switching` suite covers complete round trips, folder
relocation, graph texture references, OBJ sidecars and material slots, import
collisions, Unicode folder and asset decoding, corrupt files, failed atomic replacement, missing assets, scene
isolation, dirty tracking and renderer output paths. The fixture deliberately
includes a model without UVs to exercise the loader's tangent fallback.
