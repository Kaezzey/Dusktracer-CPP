# Editor layout and previews

The Outliner groups objects and lights and filters by name. Selecting a row opens
its Properties; Delete acts on that selection, with undo/redo. Camera, render
quality, output, denoising, and experimental caustics live in Render Settings.
The Content Drawer has separate Textures, Meshes, and Materials tabs.

The material graph has an independent sphere preview: drag to rotate, scroll to
zoom, and Reset to restore the view. It renders at 192 pixels while interacting
and refines to 512 pixels when settled. Rotation changes only preview state,
never material data or scene acceleration structures. Studio lights stay fixed
while the sphere's UVs and tangent frame rotate. This uses the actual CPU material
evaluator for direct lighting, not the final scene path tracer.

Three bounded, joined workers serve interactive spheres, material thumbnails,
and texture thumbnails separately. OpenGL uploads remain on the UI thread. Each
job owns a scene/material snapshot, and revision checks reject stale results.
Only visible drawer cells request asset thumbnails.

Disposable files in `.dusk-cache/previews` store reduced texture sources (at most
1024 pixels per side for thumbnails/spheres, 2048 for the raster viewport) and settled 128-pixel material thumbnails. File size and
modification time invalidate texture data; graph surface keys, material parameters,
and referenced texture stamps invalidate material thumbnails. Each asset replaces
its cache file on edits. Invalid/truncated files regenerate; unwritable caches
fall back to normal decoding. The final renderer still reads original textures.
An eight-image LRU per preview worker retains decoded sources between edits.
HDR preview sources retain their original float representation.

`DuskGraphIntegrationTests` exercises production ImGui dragging, independent worker
scheduling, preview refinement, cache corruption/invalidation, and Outliner actions.
It writes material-editor and Outliner screenshots in its isolated output folder.
An optional fourth argument profiles a real texture without modifying it:

```powershell
& .\build\Release\DuskGraphIntegrationTests.exe build/graph-integration profile 'C:/path/to/texture.png'
```

For the project's 53 MB chest normal map, measured preview creation took about
778 ms cold and 5.7 ms from disk cache on the development machine. These are
individual asset timings, not a measurement of complete application startup.

The scene viewport now evaluates materials on model UVs with cached shadows;
see [Raster preview](RASTER_PREVIEW.md) for its rendering and caching contract.
